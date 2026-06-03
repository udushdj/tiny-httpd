#include "server.h"
#include <strings.h>
#include <unistd.h>

char g_docroot[2048];

int main(int argc, char *argv[])
{
    int port = 8080;

    if (argc > 1) port = atoi(argv[1]);

    {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            char *base = strrchr(cwd, '/');
            if (base && strcmp(base, "/src") == 0) {
                if (chdir("..") == -1) perror("chdir");
            }
        }
    }

    if (realpath(DOCROOT, g_docroot) == NULL) {
        fprintf(stderr, "DOCROOT 不存在: %s\n", DOCROOT);
        return 1;
    }

    int listen_fd = create_listen_socket(port);
    if (listen_fd == -1) { fprintf(stderr, "无法创建监听 socket\n"); return 1; }
    printf("[INFO] 监听端口 %d，DOCROOT=%s\n", port, g_docroot);

    int epfd = epoll_create1(0);
    if (epfd == -1) { perror("epoll_create1"); close(listen_fd); return 1; }

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl ADD listen_fd");
        close(listen_fd); close(epfd); return 1;
    }

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }

        for (int i = 0; i < nfds; i++) {
            uint32_t evs = events[i].events;
            conn_t  *c   = (conn_t *)events[i].data.ptr;

            if (c == NULL) {
                if (evs & EPOLLIN) handle_accept(epfd, listen_fd);
            } else {
                if (evs & (EPOLLERR | EPOLLHUP)) {
                    close_conn(epfd, c);
                    continue;
                }
                if (evs & EPOLLIN)  handle_read(epfd, c);
                if (evs & EPOLLOUT) handle_write(epfd, c);
            }
        }
    }

    close(listen_fd);
    close(epfd);
    return 0;
}
