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

#define MAX_REQ 100000

typedef struct {
    int    id;
    char   host[64];
    int    port;
    int    total;
    int    success;
    int    fail;
    double total_ms;
    double min_ms;
    double max_ms;
} stress_thread_t;

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void *stress_worker(void *arg)
{
    stress_thread_t *st = (stress_thread_t *)arg;
    char request[512];
    snprintf(request, sizeof(request),
        "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        st->host);

    st->min_ms = 1e9;
    st->max_ms = 0;

    for (int i = 0; i < st->total; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) { st->fail++; continue; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(st->port);
        inet_pton(AF_INET, st->host, &addr.sin_addr);

        double start = now_ms();

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            st->fail++;
            close(fd);
            continue;
        }

        /* 完整发送请求 */
        size_t len = strlen(request);
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(fd, request + sent, len - sent, 0);
            if (n > 0) sent += n;
            else { st->fail++; close(fd); goto next; }
        }

        /* 读取响应 */
        char buf[4096];
        int total_read = 0;
        while (1) {
            int n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            total_read += n;
        }
        close(fd);

        double elapsed = now_ms() - start;
        st->total_ms += elapsed;
        if (elapsed < st->min_ms) st->min_ms = elapsed;
        if (elapsed > st->max_ms) st->max_ms = elapsed;

        if (total_read > 0) st->success++;
        else st->fail++;
    next:;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    char   *host   = "127.0.0.1";
    int     port   = 8080;
    int     total  = 1000;
    int     conc   = 10;

    int opt;
    while ((opt = getopt(argc, argv, "h:p:n:c:")) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'n': total = atoi(optarg); break;
            case 'c': conc = atoi(optarg); break;
            default:
                fprintf(stderr, "用法: %s [-h host] [-p port] [-n requests] [-c concurrency]\n", argv[0]);
                return 1;
        }
    }

    printf("[STRESS] 目标: http://%s:%d/\n", host, port);
    printf("[STRESS] 总请求: %d, 并发: %d 线程\n", total, conc);

    stress_thread_t *sts = calloc(conc, sizeof(stress_thread_t));
    pthread_t *tids      = malloc(sizeof(pthread_t) * conc);
    if (!sts || !tids) { fprintf(stderr, "内存不足\n"); return 1; }

    double t_start = now_ms();

    for (int i = 0; i < conc; i++) {
        sts[i].id = i;
        strncpy(sts[i].host, host, sizeof(sts[i].host) - 1);
        sts[i].port  = port;
        sts[i].total = total / conc + (i < total % conc ? 1 : 0);
        pthread_create(&tids[i], NULL, stress_worker, &sts[i]);
    }

    int ok = 0, fail = 0;
    double total_ms = 0, min_ms = 1e9, max_ms = 0;
    for (int i = 0; i < conc; i++) {
        pthread_join(tids[i], NULL);
        ok       += sts[i].success;
        fail     += sts[i].fail;
        total_ms += sts[i].total_ms;
        if (sts[i].min_ms < min_ms) min_ms = sts[i].min_ms;
        if (sts[i].max_ms > max_ms) max_ms = sts[i].max_ms;
    }

    double elapsed = now_ms() - t_start;
    double qps = (ok + fail) / (elapsed / 1000.0);

    printf("[STRESS] 完成: %d/%d\n", ok + fail, total);
    printf("[STRESS] 耗时: %.2fs, QPS: %.1f\n", elapsed/1000.0, qps);
    printf("[STRESS] 成功: %d, 失败: %d\n", ok, fail);
    if (ok > 0)
        printf("[STRESS] 最小: %.0fms, 平均: %.0fms, 最大: %.0fms\n",
               min_ms, total_ms / (ok + fail), max_ms);

    free(sts); free(tids);
    return fail > 0 ? 1 : 0;
}
