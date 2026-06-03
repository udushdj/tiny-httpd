#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_EVENTS      1024
#define MAX_CONN        1024
#define BUF_SIZE        8192
#define LISTEN_BACKLOG  128

typedef struct {
    int   fd;             /* 客户端 socket 文件描述符 */
    char  buf[BUF_SIZE];  /* 读写共用缓冲区 */
    int   len;            /* 当前缓冲区有效数据长度 */
} conn_t;

static conn_t connections[MAX_CONN];

/*
 * 将文件描述符设为非阻塞模式
 * 成功返回 0，失败返回 -1
 */
static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return -1;
    }
    return 0;
}

/*
 * 创建监听 socket，绑定到指定端口
 * 成功返回 socket fd，失败返回 -1
 */
static int create_listen_socket(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return -1;
    }

    /* 允许地址重用，避免 TIME_WAIT 导致 bind 失败 */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  /* 监听所有网卡 */
    addr.sin_port        = htons(port); /* 主机字节序 → 网络字节序 */

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, LISTEN_BACKLOG) == -1) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    if (set_nonblock(listen_fd) == -1) {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

/*
 * 处理新连接：accept → 设非阻塞 → 注册到 epoll
 */
static void handle_accept(int epfd, int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);

    /* 循环 accept，一次性取出所有就绪的连接 */
    while (1) {
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 所有就绪连接已处理完毕 */
                break;
            }
            perror("accept");
            break;
        }

        /* fd 超出管理范围则拒绝 */
        if (client_fd >= MAX_CONN) {
            fprintf(stderr, "[WARN] fd %d 超出 MAX_CONN，关闭连接\n", client_fd);
            close(client_fd);
            continue;
        }

        if (set_nonblock(client_fd) == -1) {
            close(client_fd);
            continue;
        }

        /* 初始化连接上下文 */
        conn_t *c = &connections[client_fd];
        c->fd  = client_fd;
        c->len = 0;

        /* 注册到 epoll，监听可读事件 */
        struct epoll_event ev;
        ev.events   = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            perror("epoll_ctl ADD");
            close(client_fd);
            continue;
        }

        printf("[INFO] 新连接: fd=%d, %s:%d\n",
               client_fd,
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));
    }
}

/*
 * 处理客户端可读事件：读取数据，如读取完毕则回显
 */
static void handle_read(int epfd, conn_t *c)
{
    char buf[BUF_SIZE];
    int  n = read(c->fd, buf, sizeof(buf));

    if (n == 0) {
        /* 客户端关闭连接（FIN） */
        printf("[INFO] fd=%d 客户端断开\n", c->fd);
        close_conn(epfd, c);
        return;
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 非阻塞模式下没有数据可读，正常 */
            return;
        }
        perror("read");
        close_conn(epfd, c);
        return;
    }

    /* 将读到的数据原样放入写缓冲区 */
    memcpy(c->buf, buf, n);
    c->len = n;

    /* 切换 epoll 事件：不再监听可读，改为监听可写 */
    struct epoll_event ev;
    ev.events   = EPOLLOUT;
    ev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
        perror("epoll_ctl MOD");
        close_conn(epfd, c);
    }
}

/*
 * 处理客户端可写事件：非阻塞地发送数据，发完关闭
 */
static void handle_write(int epfd, conn_t *c)
{
    int n = write(c->fd, c->buf, c->len);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 发送缓冲区满，等待下次 EPOLLOUT */
            return;
        }
        perror("write");
        close_conn(epfd, c);
        return;
    }

    if (n >= c->len) {
        /* 全部发送完毕，关闭连接 */
        printf("[INFO] fd=%d 发送完成，关闭连接\n", c->fd);
        close_conn(epfd, c);
    } else {
        /* 只发了一部分，移动剩余数据到缓冲区头部，等待下次可写 */
        memmove(c->buf, c->buf + n, c->len - n);
        c->len -= n;
    }
}

/*
 * 关闭连接：从 epoll 移除 → close(fd) → 清空结构体
 */
static void close_conn(int epfd, conn_t *c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    memset(c, 0, sizeof(conn_t));
}

int main(int argc, char *argv[])
{
    int port = 8080;

    if (argc > 1) {
        port = atoi(argv[1]);
    }

    int listen_fd = create_listen_socket(port);
    if (listen_fd == -1) {
        fprintf(stderr, "无法创建监听 socket\n");
        return 1;
    }
    printf("[INFO] 监听端口 %d，等待连接...\n", port);

    /* 创建 epoll 实例 */
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        close(listen_fd);
        return 1;
    }

    /* 将监听 fd 注册到 epoll */
    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = NULL;  /* listen_fd 不需要 conn_t */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl ADD listen_fd");
        close(listen_fd);
        close(epfd);
        return 1;
    }

    /* 事件循环 */
    struct epoll_event events[MAX_EVENTS];

    while (1) {
        /*
         * 阻塞等待事件，超时 1000ms。
         * 超时可用于后续 Day4 的超时检查。
         */
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nfds == -1) {
            if (errno == EINTR) {
                /* 被信号中断，继续等待 */
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            uint32_t evs = events[i].events;
            conn_t  *c   = (conn_t *)events[i].data.ptr;

            if (c == NULL) {
                /* data.ptr == NULL 的是 listen_fd */
                if (evs & EPOLLIN) {
                    handle_accept(epfd, listen_fd);
                }
            } else {
                /* 错误或挂起 → 关闭连接 */
                if (evs & (EPOLLERR | EPOLLHUP)) {
                    close_conn(epfd, c);
                    continue;
                }

                if (evs & EPOLLIN) {
                    handle_read(epfd, c);
                }

                if (evs & EPOLLOUT) {
                    handle_write(epfd, c);
                }
            }
        }
    }

    close(listen_fd);
    close(epfd);
    return 0;
}
