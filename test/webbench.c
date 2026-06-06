#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <getopt.h>
#include <signal.h>

#define MAX_THREADS 2000

typedef struct {
    char   host[64];
    int    port;
    int    duration;   /* 持续秒数 */
    int    success;
    int    fail;
    long   bytes;      /* 接收总字节数 */
} wb_thread_t;

static volatile int g_running = 1;
static double g_total_seconds = 0;

static void alarm_handler(int sig) {
    (void)sig;
    g_running = 0;
}

void *webbench_worker(void *arg)
{
    wb_thread_t *wt = (wb_thread_t *)arg;
    char request[512];
    snprintf(request, sizeof(request),
        "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: webbench/1.5\r\n\r\n",
        wt->host);

    signal(SIGPIPE, SIG_IGN);

    while (g_running) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) { wt->fail++; continue; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(wt->port);
        inet_pton(AF_INET, wt->host, &addr.sin_addr);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            wt->fail++;
            close(fd);
            continue;
        }

        /* 发送请求 */
        if (write(fd, request, strlen(request)) <= 0) {
            wt->fail++;
            close(fd);
            continue;
        }

        /* 读取响应 */
        char buf[1500];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            wt->bytes += n;
        }
        close(fd);

        if (n == 0) wt->success++;
        else wt->fail++;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    char   *host      = "127.0.0.1";
    int     port      = 8080;
    int     clients   = 100;
    int     duration  = 30;

    int opt;
    while ((opt = getopt(argc, argv, "h:p:c:t:")) != -1) {
        switch (opt) {
            case 'h': host     = optarg; break;
            case 'p': port     = atoi(optarg); break;
            case 'c': clients  = atoi(optarg); break;
            case 't': duration = atoi(optarg); break;
            default:
                fprintf(stderr,
                    "用法: %s [-h host] [-p port] [-c concurrent] [-t seconds]\n",
                    argv[0]);
                return 1;
        }
    }

    printf("WebBench - 压力测试\n");
    printf("目标: http://%s:%d/\n", host, port);
    printf("并发: %d 客户端\n", clients);
    printf("持续: %d 秒\n", duration);
    printf("---\n");

    /* 启动定时器 */
    alarm(duration);
    signal(SIGALRM, alarm_handler);

    wb_thread_t *threads = calloc(clients, sizeof(wb_thread_t));
    pthread_t   *tids    = malloc(sizeof(pthread_t) * clients);
    if (!threads || !tids) { fprintf(stderr, "内存不足\n"); return 1; }

    double t_start = (double)time(NULL);

    /* 创建并发客户端线程 */
    for (int i = 0; i < clients; i++) {
        strncpy(threads[i].host, host, sizeof(threads[i].host) - 1);
        threads[i].port     = port;
        threads[i].duration = duration;
        pthread_create(&tids[i], NULL, webbench_worker, &threads[i]);
    }

    /* 等待定时器触发 */
    while (g_running) {
        usleep(100000);
    }

    g_total_seconds = (double)time(NULL) - t_start;

    /* 等待所有线程退出 */
    for (int i = 0; i < clients; i++) {
        pthread_join(tids[i], NULL);
    }

    /* 汇总结果 */
    long total_success = 0, total_fail = 0, total_bytes = 0;
    for (int i = 0; i < clients; i++) {
        total_success += threads[i].success;
        total_fail    += threads[i].fail;
        total_bytes   += threads[i].bytes;
    }

    long total_req = total_success + total_fail;
    double speed   = (total_req) / g_total_seconds;
    double bw      = (double)total_bytes / g_total_seconds / 1024.0;  /* KB/s */

    printf("结果:\n");
    printf("  总请求: %ld\n", total_req);
    printf("  成功:   %ld\n", total_success);
    printf("  失败:   %ld\n", total_fail);
    printf("  速率:   %.1f requests/second\n", speed);
    printf("  带宽:   %.1f KB/s\n", bw);
    printf("  耗时:   %.2f 秒\n", g_total_seconds);

    free(threads);
    free(tids);
    return total_fail > 0 ? 1 : 0;
}
