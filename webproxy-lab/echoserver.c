#include "csapp.h"

void echo(int connfd);

int main(int argc, char **argv) {
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];

    if(argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[1]);
    }

    /*
    Passive mode로 open socket
    */
    listenfd = Open_listenfd(argv[1]);
    while(1) {
        clientlen = sizeof(struct sockaddr_storage);
        // printf("before accecpt...\n");
        /*
        client연결 요청 들어올 때까지 대기
        */
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        // printf("after accept!\n");
        Getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
        printf("Connectd to (%s, %s)\n", client_hostname, client_port);
        /*
        accept되어 return된 connected socket으로 Echo실행
        */
        echo(connfd);
        Close(connfd);
    }
    
    exit(0);
}

void echo(int connfd) {
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    /*
    rio 객체 초기화
    */
    Rio_readinitb(&rio, connfd);
    /* connfd에 들어온 버퍼 EOF까지 읽고 buf에 담기 */
    while((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        printf("server received %d bytes\n", (int)n);
        Rio_writen(connfd, buf, n);
    }
}