#include "server.h"
#include <getopt.h>
#include <unistd.h>

char g_docroot[2048];

/*
    --打印终端信息的工具函数，输出错误文件名字及信息
    --prog：    通常是 argv[0]，即可执行程序的名字（如 ./myhttpd），
                动态传入可保证输出中始终显示正确的程序名。
*/
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "  -p PORT     监听端口 (默认 8080)\n"
        "  -t N        worker 线程数 (默认 1)\n"
        "  -d DIR      DOCROOT 目录 (默认 ./www)\n"
        "  -l DIR      日志目录 (默认 logs)\n"
        "  -h          显示帮助\n", prog);
}

int main(int argc, char *argv[])
{
    int port       = 8080;          //   端口
    int workers    = 1;             //   线程数
    const char *docroot = "./www";  //   根目录
    const char *logdir  = "logs";   //   日志目录

    int opt;    // 短选项
    //  短选项解析函数getopt
    while ((opt = getopt(argc, argv, "p:t:d:l:h")) != -1) {
        switch (opt) {
            case 'p': port     = atoi(optarg); break;
            case 't': workers  = atoi(optarg); break;
            case 'd': docroot  = optarg;       break;
            case 'l': logdir   = optarg;       break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    /* 
        让进程忽略 SIGPIPE 信号，
        防止在对端已关闭的连接上执行写操作时，
        进程被系统直接杀死。
    */
    signal(SIGPIPE, SIG_IGN);

    /* 解析根目录DOCROOT */
    { 
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {     //  获取当前进程的工作目录
            char *base = strrchr(cwd, '/');  // 在一个字符串中查找某个字符最后一次出现的位置
            if (base && strcmp(base, "/src") == 0) {
                if (chdir("..") == -1) perror("chdir");  // chdir改变进程当前工作目录的函数
            }
        }
    }

    if (realpath(docroot, g_docroot) == NULL) {
        fprintf(stderr, "DOCROOT 不存在: %s\n", docroot);
        return 1;
    }

    /* 初始化日志系统 */
    g_logger = logger_init(logdir, LOG_INFO, LOG_TARGET_BOTH);
    if (g_logger) {
        log_server(g_logger, LOG_INFO, "日志系统初始化完成, 目录: %s", logdir); 
    }

    /* 初始化共享状态 */
    pthread_mutex_init(&g_stats.lock, NULL);
    g_stats.start_time   = time(NULL);
    g_stats.conn_count   = 0;
    g_stats.req_total    = 0;

    printf("[INFO] 监听端口 %d，DOCROOT=%s\n", port, g_docroot);
    log_server(g_logger, LOG_INFO, "服务器启动, 端口 %d", port);

    //  创建套接字
    int listen_fd = create_listen_socket(port);
    if (listen_fd == -1) { fprintf(stderr, "无法创建监听 socket\n"); return 1; }

    //  创建 epoll 实例
    int epfd = epoll_create1(0);
    if (epfd == -1) { perror("epoll_create1"); close(listen_fd); return 1; }

    //  将监听套接字加入 epoll 监听
    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl ADD listen_fd");
        close(listen_fd); close(epfd); return 1;
    }

    //  写日志
    log_server(g_logger, LOG_INFO, "epoll 事件循环启动"); 

    //  从 epoll 获取就绪事件的数组
    struct epoll_event events[MAX_EVENTS];
    /*  
        一个计数器，用于在事件循环中周期性地执行某些维护任务
        （例如检查超时连接、日志轮转检查、刷新缓存等）
    */
    int timeout_check_counter = 0;

    /*
        这正是高性能网络服务器（如 Nginx）的经典骨架：
        无限循环等待事件 → 分类处理 → 定期打扫垃圾。
    */
    while (1) {
        /*  
            阻塞等待事件发生，最多等待 1000 毫秒（1 秒）。
            即使没有任何事件，也会超时返回 0，
            让循环有机会执行后面的周期性任务。
        */
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        /*  
            返回值 nfds：就绪的文件描述符数量。
            nfds == -1：如果 errno 是 EINTR（被信号中断），
            直接 continue 重新等待；否则打印错误并跳出循环，
            通常意味着服务器即将关闭。
            nfds == 0：超时，跳过后面的 for 循环，直接进入周期性任务。
        */
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }

        for (int i = 0; i < nfds; i++) {
            uint32_t evs = events[i].events;
            conn_t  *c   = (conn_t *)events[i].data.ptr;
                
            if (c == NULL) {
                /* 
                    如果 c 是 NULL，说明是监听套接字上的事件。
                    只有 EPOLLIN 有意义（新连接到来），
                    调用 handle_accept 接受连接。
                */    
                if (evs & EPOLLIN) handle_accept(epfd, listen_fd);
            } else {
                /*
                    如果连接处于 STATE_CGI 状态，
                    说明它正在处理一个 CGI 请求。
                */
                if (c->state == STATE_CGI) {
                    /* CGI 管道事件 */
                    if (evs & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                        handle_cgi_read(epfd, c);
                    }
                } else {
                    /* 普通 HTTP 连接 */
                    /*
                        先检测异常：EPOLLERR 或 EPOLLHUP 意味着连接出错或对方关闭，
                        直接调用 close_conn 清理资源。
                        否则，根据事件类型调用对应的处理函数：
                    */
                    if (evs & (EPOLLERR | EPOLLHUP)) {
                        close_conn(epfd, c);
                        continue;
                    }
                    //EPOLLIN：套接字可读，调用 handle_read 解析 HTTP 请求。
                    if (evs & EPOLLIN)  handle_read(epfd, c);
                    //EPOLLOUT：套接字可写，调用 handle_write 发送 HTTP 响应。
                    if (evs & EPOLLOUT) handle_write(epfd, c);
                }
            }
        }

        /* 每 10 次 epoll_wait 检查一次超时（约 10 秒） */
        if (++timeout_check_counter >= 10) {
            timeout_check_counter = 0;
            check_timeout(epfd);
            reap_zombies();
        }
    }

    log_server(g_logger, LOG_INFO, "服务器退出");
    if (g_logger) logger_close(g_logger);
    pthread_mutex_destroy(&g_stats.lock);
    close(listen_fd);
    close(epfd);
    return 0;
}
