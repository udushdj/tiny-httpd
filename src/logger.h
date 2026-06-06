#ifndef LOGGER_H
#define LOGGER_H

/**
 * @file    logger.h
 * @brief   分级日志系统 — 线程安全、自动轮转、可编译期裁剪
 *
 * 提供三个独立日志文件：
 *   - access.log   每个 HTTP 请求的方法/URI/状态码/耗时
 *   - server.log   服务器生命周期事件（启动/停止/配置）
 *   - error.log    仅错误信息（同时写入 server.log）
 *
 * 特性：
 *   1. 线程安全  — 所有写文件操作由 pthread_mutex_t 保护
 *   2. 自动轮转  — 单文件超 10MB 时自动 .2→删除 .1→.2 当前→.1
 *   3. 编译裁剪  — 宏 ENABLE_LOG=0 时整个日志系统被优化剔除
 *   4. 双目标     — 支持仅控制台、仅文件、同时输出三种模式
 *   5. 级别过滤  — 低于 min_level 的日志自动丢弃               */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

/* ──── 编译宏默认值（可在 CMakeLists.txt 中覆盖） ──── */
#ifndef ENABLE_LOG
#define ENABLE_LOG 1              /* 设为 0 完全禁用日志系统，消除运行时开销 */
#endif
#ifndef LOG_DIR
#define LOG_DIR "logs"            /* 默认日志输出目录 */
#endif
#ifndef LOG_MAX_SIZE
#define LOG_MAX_SIZE (10*1024*1024)  /* 单个文件轮转阈值：10MB */
#endif

/* ════════════════════════════════════════════════════════
   日志级别
   ════════════════════════════════════════════════════════
   级别从低到高排列。设置 min_level 后，低于该级别的消息
   会被直接丢弃。例如设置为 LOG_WARN 则 DEBUG/INFO 均不记录。  */

typedef enum {
    LOG_DEBUG = 0,   /* 调试信息：变量值、分支走向，仅开发期开启     */
    LOG_INFO  = 1,   /* 一般信息：连接建立、请求处理、正常流程       */
    LOG_WARN  = 2,   /* 警告：连接超时、资源接近上限、可恢复异常     */
    LOG_ERROR = 3    /* 错误：连接失败、系统调用异常、CGI 崩溃       */
} log_level_t;

/* ════════════════════════════════════════════════════════
   日志输出目标
   ════════════════════════════════════════════════════════ */

typedef enum {
    LOG_TARGET_CONSOLE = 0,  /* 仅打印到 stdout/stderr（嵌入式调试） */
    LOG_TARGET_FILE    = 1,  /* 仅写入文件（生产环境，减少终端干扰） */
    LOG_TARGET_BOTH    = 2   /* 同时输出到终端和文件（开发调试）     */
} log_target_t;

/* ════════════════════════════════════════════════════════
   日志系统上下文
   ════════════════════════════════════════════════════════
   全局单例（g_logger），由 logger_init() 分配，logger_close() 释放。
   所有日志函数通过此结构访问文件句柄和配置。                    */

typedef struct {
    FILE       *access_fp;       /* access.log   — HTTP 请求记录  */
    FILE       *server_fp;       /* server.log   — 服务器事件      */
    FILE       *error_fp;        /* error.log    — 错误专用        */
    char        log_dir[256];    /* 日志目录路径                    */
    log_level_t min_level;       /* 最低记录级别，低于此值的忽略    */
    log_target_t target;         /* 输出方向：终端/文件/双写        */
    size_t      max_size;        /* 轮转阈值（默认 10MB）           */
    pthread_mutex_t lock;        /* 互斥锁，保护并发写入            */
} logger_t;

/* ════════════════════════════════════════════════════════
   API 声明
   ════════════════════════════════════════════════════════ */

/*
 * 创建并配置日志记录器
 * -- log_dir:  存放日志文件的目录路径
 * -- level:    最低记录等级（低于此值忽略）
 * -- target:   输出目标（终端/文件/双写）
 * 返回 NULL 表示内存不足或 ENABLE_LOG=0
 */
logger_t *logger_init(const char *log_dir, log_level_t level, log_target_t target);

/*
 * 写入一条通用日志到 server.log（同时可选控制台输出）
 * 线程安全，内部自动轮转检查
 */
void log_write(logger_t *l, log_level_t level, const char *fmt, ...);

/*
 * 写入请求日志到 access.log
 * ip/client:  客户端地址
 * method:     GET/POST
 * uri:        请求路径
 * status:     HTTP 状态码
 * elapsed_ms: 处理耗时（毫秒）
 * 注意：纯控制台模式（LOG_TARGET_CONSOLE）下本函数不输出
 */
void log_access(logger_t *l, const char *ip, const char *method,
                const char *uri, int status, int elapsed_ms);

/*
 * 写入服务器事件日志到 server.log
 * 与 log_write 功能相同，语义更明确——用于记录服务器级别事件
 */
void log_server(logger_t *l, log_level_t level, const char *fmt, ...);

/*
 * 写入错误日志 — 同时写入 server.log 和 error.log
 * func_name: 发生错误的函数名（用于定位）
 * fmt...:    错误描述（printf 格式）
 */
void log_error(logger_t *l, const char *func_name, const char *fmt, ...);

/*
 * 检查三个日志文件大小，超过 max_size 则触发轮转
 * 轮转策略：.2→删除，.1→.2，当前→.1
 * 在每次写文件前调用（内部自动，无需手动）
 */
void log_rotate_check(logger_t *l);

/*
 * 关闭日志系统：关闭所有文件句柄，释放互斥锁和内存
 */
void logger_close(logger_t *l);

/* ════════════════════════════════════════════════════════
   内联辅助函数
   ════════════════════════════════════════════════════════ */

/*
 * 日志级别 → 可读字符串
 */
static inline const char *log_level_str(log_level_t level)
{
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        default:        return "UNKNOWN";
    }
}

/*
 * 获取当前时间字符串: "2026-06-04 16:30:00"
 * buf 至少需要 20 字节
 */
static inline void log_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm);
}

#endif
