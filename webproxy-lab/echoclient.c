#include "csapp.h"

int main(int argc, char **argv) {
    int clientfd;
    char *host, *port, buf[MAXLINE];
    rio_t rio;

    if(argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0);
    }
    host = argv[1];
    port = argv[2];

    clientfd = Open_clientfd(host, port); /* client 열기 */
    Rio_readinitb(&rio, clientfd); /* rio객체에 client socket 정보 담기 */

    while (Fgets(buf, MAXLINE, stdin) != NULL) /* stdin에서 버퍼 비워질때까지 기다리기 */
    {
        // printf("[client]: write\n");
        Rio_writen(clientfd, buf, strlen(buf)); /* socket에 buf write */
        Rio_readlineb(&rio, buf, MAXLINE); /* rio에 들어온 eof까지 읽은 버퍼 복사 */
        Fputs(buf, stdout); /* stdout에 버퍼 출력 */
    }
    Close(clientfd); /* close socket */
    exit(0);
}