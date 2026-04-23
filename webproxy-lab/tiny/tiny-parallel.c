/*
 * tiny-parallel.c - thread pool을 사용해 여러 HTTP 요청을 병렬로 처리하는 Tiny 서버
 *
 * 실행 방법:
 *   ./tiny-parallel <port>
 *
 * 구조:
 *   - main thread는 accept()로 새 연결을 받아 bounded buffer에 넣는다.
 *   - worker thread들은 buffer에서 연결 fd를 꺼내 doit()으로 요청을 처리한다.
 *   - 각 연결 fd는 요청 처리가 끝난 worker thread가 직접 닫는다.
 */
#include "csapp.h"

#define NTHREADS 8 /* 동시에 요청을 처리할 작업 thread 개수 */
#define SBUFSIZE 16 /* 대기 중인 연결 fd를 저장할 bounded buffer 크기 */

typedef struct
{
  int *buf;                 /* 연결 fd를 담는 원형 배열 */
  int n;                    /* buf 슬롯 개수 */
  int front;                /* 다음에 꺼낼 위치 */
  int rear;                 /* 다음에 넣을 위치 */
  int count;                /* 현재 buffer에 들어 있는 fd 개수 */
  pthread_mutex_t mutex;    /* front/rear/count 접근 보호 */
  pthread_cond_t not_full;  /* 빈 슬롯이 생겼음을 알림 */
  pthread_cond_t not_empty; /* 처리할 fd가 들어왔음을 알림 */
} sbuf_t;

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);
void sbuf_init(sbuf_t *sp, int n);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);
void *thread(void *vargp);

sbuf_t sbuf; /* main thread와 작업 thread들이 공유하는 연결 fd 큐 */

int main(int argc, char **argv)
{
  int listenfd, connfd;
  pthread_t tid;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  int i;

  /* 명령행 인자 검사 */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]); /* 연결 요청을 기다릴 listen socket 열기 */
  sbuf_init(&sbuf, SBUFSIZE);

  /* 작업 thread들을 미리 생성한다. 각 작업 thread는 thread() 안에서 무한 반복한다. */
  for (i = 0; i < NTHREADS; i++)
    Pthread_create(&tid, NULL, thread, NULL);

  while (1)
  {
    /* 클라이언트 주소 구조체 크기 */
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr,
                    &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);

    /*
     * main thread는 요청을 직접 처리하지 않고 큐에 넣기만 한다.
     * 큐가 가득 차면 worker가 fd를 꺼낼 때까지 여기서 대기한다.
     */
    sbuf_insert(&sbuf, connfd);
  }
}

/* 위에서 선언한 함수들의 구현 */

/*
 * sbuf_init - bounded buffer 초기화
 */
void sbuf_init(sbuf_t *sp, int n)
{
  sp->buf = Calloc(n, sizeof(int));
  sp->n = n;
  sp->front = sp->rear = 0;
  sp->count = 0;
  pthread_mutex_init(&sp->mutex, NULL);
  pthread_cond_init(&sp->not_full, NULL);
  pthread_cond_init(&sp->not_empty, NULL);
}

/*
 * sbuf_insert - 연결 fd를 bounded buffer 뒤쪽에 넣는다.
 */
void sbuf_insert(sbuf_t *sp, int item)
{
  pthread_mutex_lock(&sp->mutex);
  while (sp->count == sp->n)
    pthread_cond_wait(&sp->not_full, &sp->mutex); /* 빈 슬롯이 생길 때까지 대기 */

  sp->buf[(++sp->rear) % sp->n] = item;
  sp->count++;
  pthread_cond_signal(&sp->not_empty); /* 잠든 작업 thread 하나를 깨움 */
  pthread_mutex_unlock(&sp->mutex);
}

/*
 * sbuf_remove - bounded buffer 앞쪽에서 연결 fd를 꺼낸다.
 */
int sbuf_remove(sbuf_t *sp)
{
  int item;

  pthread_mutex_lock(&sp->mutex);
  while (sp->count == 0)
    pthread_cond_wait(&sp->not_empty, &sp->mutex); /* 처리할 연결이 들어올 때까지 대기 */

  item = sp->buf[(++sp->front) % sp->n];
  sp->count--;
  pthread_cond_signal(&sp->not_full); /* accept thread가 큐에 넣을 수 있게 알림 */
  pthread_mutex_unlock(&sp->mutex);
  return item;
}

/*
 * thread - worker thread가 실행하는 함수
 */
void *thread(void *vargp)
{
  int connfd;

  Pthread_detach(Pthread_self()); /* main thread가 join하지 않아도 자원 회수 */
  while (1)
  {
    connfd = sbuf_remove(&sbuf);
    doit(connfd);  /* 요청/응답 처리 */
    Close(connfd); /* fd 소유권은 worker에게 있으므로 worker가 닫는다. */
  }
}


/*
 * doit - HTTP 요청 하나를 읽고 응답 하나를 보낸다.
 */
void doit(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

  /* 요청 라인과 요청 헤더 읽기 */
  Rio_readinitb(&rio, fd);
  if (!Rio_readlineb(&rio, buf, MAXLINE))
    return;
  sscanf(buf, "%s %s %s", method, uri, version);
  printf("%s", buf);
  if (strcasecmp(method, "GET")) /* 대소문자 구분없이 비교한다. 같으면 0을 반환한다. */
  {
    clienterror(fd, method, "501", "Not Implemented",
                "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio);

  /* GET 요청 URI를 파일 이름과 CGI 인자로 분리 */
  is_static = parse_uri(uri, filename, cgiargs);
  /* 
    stat(): system call. pathname을 기반으로 파일의 메타데이터를 struct stat에 저장.
    성공시 0, 실패시 -1 return
  */
  if (stat(filename, &sbuf) < 0)
  {
    clienterror(fd, filename, "404", "Not found",
                "Tiny couldn't find this file");
    return;
  }

  if (is_static)
  { /* 정적 컨텐츠 처리 */
    // 일반 파일인지 확인 || 파일 소유자에게 읽기 권한이 있는지 확인
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
    {
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn't read the file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size);
  }
  else
  { /* 동적 컨텐츠 처리 */
    // 일반 파일인지 확인 || 파일 소유자에게 실행권한이 있는지 확인
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode))
    {
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs);
  }
}

/*
 * read_requesthdrs - HTTP 요청 헤더를 끝까지 읽는다.
 */
void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  printf("%s", buf);
  while (strcmp(buf, "\r\n"))
  {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

/*
 * parse_uri - URI를 파일 이름과 CGI 인자로 나눈다.
 *             동적 컨텐츠면 0, 정적 컨텐츠면 1을 반환한다.
 */
int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  if (!strstr(uri, "cgi-bin"))
  { /* 정적 컨텐츠 */
    strcpy(cgiargs, "");
    strcpy(filename, ".");
    strcat(filename, uri);
    if (uri[strlen(uri) - 1] == '/')
      strcat(filename, "home.html");
    return 1;
  }
  else
  { /* 동적 컨텐츠 */
    ptr = index(uri, '?');
    if (ptr)
    {
      strcpy(cgiargs, ptr + 1);
      *ptr = '\0';
    }
    else
      strcpy(cgiargs, "");
    strcpy(filename, ".");
    strcat(filename, uri);
    return 0;
  }
}

/*
 * serve_static - 정적 파일 내용을 클라이언트로 보낸다.
 */
void serve_static(int fd, char *filename, int filesize)
{
  int srcfd;
  char *srcp, filetype[MAXLINE];

  char buf[MAXBUF];
  char *p = buf;
  int n;
  int remaining = sizeof(buf);

  /* 응답 헤더를 클라이언트로 보낸다. */
  get_filetype(filename, filetype);

  /* HTTP 응답 헤더를 하나의 버퍼에 차례대로 만든다. */
  n = snprintf(p, remaining, "HTTP/1.0 200 OK\r\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "Server: Tiny Web Server\r\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "Connection: close\r\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "Content-length: %d\r\n", filesize);
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "Content-type: %s\r\n\r\n", filetype);
  p += n;
  remaining -= n;

  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n");
  printf("%s", buf);

  /* 응답 본문을 클라이언트로 보낸다. */
  srcfd = Open(filename, O_RDONLY, 0);
  srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  Close(srcfd);
  Rio_writen(fd, srcp, filesize);
  Munmap(srcp, filesize);
}

/*
 * get_filetype - 파일 이름에서 Content-Type을 결정한다.
 */
void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else
    strcpy(filetype, "text/plain");
}

/*
 * serve_dynamic - CGI 프로그램을 실행해 동적 응답을 만든다.
 */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
  char buf[MAXLINE], *emptylist[] = {NULL};
  pid_t pid;

  /* HTTP 응답의 첫 부분을 먼저 보낸다. */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  /* CGI 프로그램 실행을 담당할 자식 프로세스를 만든다. */
  if ((pid = Fork()) < 0)
  { /* fork 실패 */
    perror("Fork failed");
    return;
  }

  if (pid == 0)
  { /*
      자식 프로세스
      1. fork 후에는 open file description이 공유된다.
      2. STDOUT_FILENO를 fd로 변경한다.
      3. exec 함수로 현재 프로세스 이미지를 CGI 프로그램으로 덮어쓴다.
    */
    /* 실제 서버라면 필요한 CGI 환경변수를 더 설정한다. */
    /*
     * QUERY_STRING은 자식 프로세스에서만 설정한다.
     * thread들이 공유하는 부모 프로세스의 environ을 수정하면 CGI 요청끼리 경합할 수 있다.
     */
    setenv("QUERY_STRING", cgiargs, 1);

    /* 표준 출력을 클라이언트 연결 fd로 돌린다. */
    if (Dup2(fd, STDOUT_FILENO) < 0)
    {
      perror("Dup2 error");
      exit(1);
    }
    Close(fd);

    /* CGI 프로그램 실행 */
    /* exec함수는 fork와 달리 이 프로세스가 그대로 exec호출 프로세스로 덮어쓰여진다 */
    Execve(filename, emptylist, environ);

    /* 여기까지 오면 Execve가 실패한 것이다. */
    perror("Execve error");
    exit(1);
  }
  else
  { /* 부모 프로세스 */
    /* 부모는 CGI 자식 프로세스가 끝날 때까지 기다린다. */
    int status;
    if (waitpid(pid, &status, 0) < 0)
    {
      perror("Wait error");
    }

    printf("Child process %d terminated with status %d\n", pid, status);
    /* 부모는 정상적으로 doit()로 돌아간다. */
  }
  /* 여기서 반환하면 thread()가 연결 fd를 닫는다. */
}

/*
 * clienterror - 에러 응답을 클라이언트로 보낸다.
 */
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /* HTTP 응답 본문 생성 */
  snprintf(body, sizeof(body),
           "<html><title>Tiny Error</title>"
           "<body bgcolor=\"ffffff\">\r\n"
           "%s: %s\r\n"
           "<p>%s: %s\r\n"
           "<hr><em>The Tiny Web server</em>\r\n",
           errnum, shortmsg, longmsg, cause);

  /* HTTP 응답 전송 */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}
