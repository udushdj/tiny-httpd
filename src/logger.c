#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

#include "logger.h"

/* ════════════════════════════════════════════════════════
   内部辅助函数
   ════════════════════════════════════════════════════════ */

/*
 * 确保日志目录存在 — 不存在则创建（权限 0755）
 * stat() 成功 → 目录已存在 → 返回 0
 * stat() 失败 → 调用 mkdir → 返回 mkdir 结果
 */
static int ensure_log_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 0;
    return mkdir(path, 0755);
}

/*
 * 获取已打开文件当前大小（字节）
 * 通过 fseek(SEEK_END) 获取末尾偏移量，再恢复原位
 * 用于日志轮转前的超限判断
 */
static size_t file_size(FILE *fp)
{
    if (!fp) return 0;
    long pos = ftell(fp);
    fseek(fp, 0, SEEK_END);
    size_t sz = (size_t)ftell(fp);
    fseek(fp, pos, SEEK_SET);
    return sz;
}

/*
 * 单个日志文件轮转
 * 策略（三代保留）：
 *   path.2 → 删除
 *   path.1 → 重命名为 path.2
 *   path   → 重命名为 path.1
 * 下次写文件时由于 path 已不存在，fopen("a") 会自动创建新文件
 */
static void rotate_file(const char *path, size_t max_size)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if ((size_t)st.st_size < max_size) return;

    /* .2 → 删除, .1 → .2, 当前 → .1 */
    char old2[512], old1[512];
    snprintf(old2, sizeof(old2), "%s.2", path);
    snprintf(old1, sizeof(old1), "%s.1", path);
    unlink(old2);
    rename(old1, old2);
    rename(path, old1);
}

/* ════════════════════════════════════════════════════════
   公共 API
   ════════════════════════════════════════════════════════ */

/*
    --创建并配置日志记录器，打开记录器并预备记录
    --log_dir： 存放日志文件的路径
    --level:    最低的日志记录等级
    --target:   日志输出目标
*/
logger_t *logger_init(const char *log_dir, log_level_t level, log_target_t target)
{

    /*
        如果编译时定义了 ENABLE_LOG=0 或未定义，整个日志功能被完全禁用。
        为了消除编译器"未使用参数"的警告，用 (void) 显式忽略三个参数，然后直接返回 NULL。
        后续代码中使用该记录器时，只需检查是否为 NULL 即可安全地跳过所有日志操作。
    */
#if !ENABLE_LOG
    (void)log_dir; (void)level; (void)target;
    return NULL;
#else
    logger_t *l = calloc(1, sizeof(logger_t));
    if (!l) return NULL;

    //配置传进来的参数，并初始化同步锁
    strncpy(l->log_dir, log_dir, sizeof(l->log_dir) - 1);
    l->min_level = level;
    l->target    = target;
    l->max_size  = LOG_MAX_SIZE;
    pthread_mutex_init(&l->lock, NULL);

    //调用辅助函数确认目录是否存在
    if (ensure_log_dir(log_dir) == -1) {
        fprintf(stderr, "[WARN] 无法创建日志目录: %s (%s)\n", log_dir, strerror(errno));
    }

    //打开三个日志文件（追加模式）
    char path[512];
    snprintf(path, sizeof(path), "%s/access.log", log_dir);
    l->access_fp = fopen(path, "a");

    snprintf(path, sizeof(path), "%s/server.log", log_dir);
    l->server_fp = fopen(path, "a");

    snprintf(path, sizeof(path), "%s/error.log", log_dir);
    l->error_fp = fopen(path, "a");

    return l;
#endif
}

/*
 * 通用日志写入 — 底层的格式化+输出引擎
 * log_server() 和 log_error() 最终都走类似的格式化流程
 */
void log_write(logger_t *l, log_level_t level, const char *fmt, ...)
{
#if !ENABLE_LOG
    (void)l; (void)level; (void)fmt;
    return;
#else
    if (!l || level < l->min_level) return;

    char ts[32];
    log_timestamp(ts, sizeof(ts));

    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (l->target != LOG_TARGET_FILE) {
        FILE *out = (level >= LOG_WARN) ? stderr : stdout;
        fprintf(out, "[%s] [%s] %s\n", ts, log_level_str(level), msg);
    }

    if (l->target != LOG_TARGET_CONSOLE && l->server_fp) {
        pthread_mutex_lock(&l->lock);
        log_rotate_check(l);
        fprintf(l->server_fp, "[%s] [%s] %s\n", ts, log_level_str(level), msg);
        fflush(l->server_fp);
        pthread_mutex_unlock(&l->lock);
    }
#endif
}

/*
 * 请求访问日志 — 记录每个 HTTP 请求的关键信息
 * 格式: "[时间] 客户端IP 方法 URI 状态码 耗时ms"
 * 仅写入 access.log，不输出到控制台（避免终端刷屏）
 */
void log_access(logger_t *l, const char *ip, const char *method,
                const char *uri, int status, int elapsed_ms)
{
#if !ENABLE_LOG
    (void)l; (void)ip; (void)method; (void)uri; (void)status; (void)elapsed_ms;
    return;
#else
    if (!l || l->target == LOG_TARGET_CONSOLE) return;

    char ts[32];
    log_timestamp(ts, sizeof(ts));

    pthread_mutex_lock(&l->lock);
    log_rotate_check(l);
    if (l->access_fp) {
        fprintf(l->access_fp, "[%s] %s %s %s %d %dms\n",
                ts, ip, method, uri, status, elapsed_ms);
        fflush(l->access_fp);
    }
    pthread_mutex_unlock(&l->lock);
#endif
}

/*
    -- 函数用于向 server.log 写入一条日志（同时可输出到控制台）
    -- l ：            日志记录器指针
    -- level :         日志信息的等级
    -- fmt及... :      printf 风格的格式字符串和可变参数，用于生成最终日志消息
*/
void log_server(logger_t *l, log_level_t level, const char *fmt, ...)
{
    //同上安全跳过日志操作的宏
#if !ENABLE_LOG
    (void)l; (void)level; (void)fmt;
    return;
#else
    if (!l || level < l->min_level) return;  // 等级低于最低则直接返回

    char ts[32];
    log_timestamp(ts, sizeof(ts));  //获取时间的辅助函数

    //格式化消息
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /*
        只要 target 不是"纯文件模式"
        （例如 LOG_TARGET_BOTH 或 LOG_TARGET_CONSOLE），就打印到终端。
    */
    if (l->target != LOG_TARGET_FILE) {
        FILE *out = (level >= LOG_WARN) ? stderr : stdout; //选择文件输出流向
        fprintf(out, "[%s] [%s] %s\n", ts, log_level_str(level), msg);
    }

    /*
        当目标不是纯控制台模式，
        并且 server_fp（在 logger_init 中打开的 server.log 的文件指针）不为空时，写入文件。
    */
    if (l->target != LOG_TARGET_CONSOLE && l->server_fp) {
        /*
        线程安全：
            使用 pthread_mutex_lock 获取锁，防止多个线程同时写入导致日志内容交错。
            写完后调用 fflush 立即将缓冲区的数据刷入磁盘，降低进程崩溃时丢日志的风险（代价是性能略降）。
            pthread_mutex_unlock 释放锁。
        */
        pthread_mutex_lock(&l->lock);
        log_rotate_check(l);
        fprintf(l->server_fp, "[%s] [%s] %s\n", ts, log_level_str(level), msg);
        fflush(l->server_fp);
        pthread_mutex_unlock(&l->lock);
    }
#endif
}

/*
 * 错误日志 — 同时写入 server.log 和 error.log
 * 与 log_server 的关键区别：
 *   1. 额外写入 error.log（专用于错误追踪）
 *   2. 自动追加函数名 func_name 便于定位
 *   3. 级别固定为 ERROR，不受 min_level 过滤（错误必须记录）
 */
void log_error(logger_t *l, const char *func_name, const char *fmt, ...)
{
#if !ENABLE_LOG
    (void)l; (void)func_name; (void)fmt;
    return;
#else
    if (!l) return;

    char ts[32];
    log_timestamp(ts, sizeof(ts));

    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (l->target != LOG_TARGET_FILE) {
        fprintf(stderr, "[%s] [ERROR] %s() %s\n", ts, func_name, msg);
    }

    if (l->target != LOG_TARGET_CONSOLE) {
        pthread_mutex_lock(&l->lock);
        log_rotate_check(l);
        if (l->error_fp) {
            fprintf(l->error_fp, "[%s] [ERROR] %s() %s\n", ts, func_name, msg);
            fflush(l->error_fp);
        }
        if (l->server_fp) {
            fprintf(l->server_fp, "[%s] [ERROR] %s() %s\n", ts, func_name, msg);
            fflush(l->server_fp);
        }
        pthread_mutex_unlock(&l->lock);
    }
#endif
}

/*
    --  日志系统中负责检查并执行日志轮转（log rotation）的函数。
        它的目标非常简单：防止 access.log、server.log、error.log
        这三个日志文件无限制地增大，最终占满磁盘。
    -- l ：  日志记录器指针
*/
void log_rotate_check(logger_t *l)
{
    // 空指针保护
    if (!l) return;
    // 只有对应文件成功打开过才处理
    if (l->access_fp) {
        char path[512];
        snprintf(path, sizeof(path), "%s/access.log", l->log_dir);
        rotate_file(path, l->max_size);  //辅助函数实现日志轮转
    }
    if (l->server_fp) {
        char path[512];
        snprintf(path, sizeof(path), "%s/server.log", l->log_dir);
        rotate_file(path, l->max_size);
    }
    if (l->error_fp) {
        char path[512];
        snprintf(path, sizeof(path), "%s/error.log", l->log_dir);
        rotate_file(path, l->max_size);
    }
}

/*
 * 关闭日志系统 — 服务器退出时调用
 * 关闭全部文件句柄，销毁互斥锁，释放 logger_t 内存
 */
void logger_close(logger_t *l)
{
    if (!l) return;
    if (l->access_fp) fclose(l->access_fp);
    if (l->server_fp) fclose(l->server_fp);
    if (l->error_fp) fclose(l->error_fp);
    pthread_mutex_destroy(&l->lock);
    free(l);
}
