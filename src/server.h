#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "http_parser.h"

#define MAX_EVENTS      1024
#define BUF_SIZE        8192
#define LISTEN_BACKLOG  128
#define MAX_CONN        256
#define DOCROOT         "./www"

extern char g_docroot[2048];

enum { STATE_READING, STATE_WRITING };

struct conn_t {
    int   fd;
    int   state;
    char  rbuf[BUF_SIZE];
    int   rpos;
    int   rlen;
    char  wbuf[BUF_SIZE];
    int   wpos;
    int   wlen;

    http_parser  parser;
    http_request request;
};

extern conn_t *g_conns[MAX_CONN];

int   create_listen_socket(int port);
int   set_nonblock(int fd);

void  handle_accept(int epfd, int listen_fd);
void  handle_read(int epfd, conn_t *c);
void  handle_write(int epfd, conn_t *c);
void  close_conn(int epfd, conn_t *c);

void  mod_epoll(int epfd, conn_t *c, int events);

void  send_error(int epfd, conn_t *c, int code, const char *msg);
void  serve_file(int epfd, conn_t *c, const char *filepath, struct stat *st);
void  exec_cgi(int epfd, conn_t *c, const char *script_path);

void  route_request(int epfd, conn_t *c);

const char *get_mime_type(const char *path);

#endif
