#ifndef SERVER_H
#define SERVER_H

/**
 * @file    server.h
 * @brief   tiny-httpd HTTP 引擎核心 — 数据结构、常量与接口声明
 *
 * 本文件定义了轻量化 HTTP 服务器的全部核心数据结构与 API：
 *   - epoll 事件驱动架构的连接状态机
 *   - 异步非阻塞 CGI 执行器上下文
 *   - 服务器全局统计与日志句柄
 *   - 请求路由、静态文件服务、错误响应等对外接口
 *
 * 设计原则：
 *   1. 单线程 epoll + 非阻塞 I/O（one-loop-per-process）
 *   2. 连接池采用指针数组 + 按需堆分配（避免 BSS 膨胀）
 *   3. CGI 子进程 stdout 管道注册到 epoll，父进程不阻塞等待
 *   4. 零外部依赖，仅依赖 POSIX 系统调用和标准 C 库
 */

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
#include <signal.h>
#include <pthread.h>
#include <time.h>

#include "http_parser.h"   /* HTTP/1.1 请求解析状态机 */
#include "logger.h"        /* 分级日志系统 + 自动轮转 */

/* ═══════════════════════════════════════════════════════════════
   系统常量
   ═══════════════════════════════════════════════════════════════ */

#define MAX_EVENTS      1024   /* epoll_wait 单次返回的最大事件数        */
#define BUF_SIZE        8192   /* 单连接读写缓冲区大小 (8KB)             */
#define LISTEN_BACKLOG  128    /* listen() 内核半连接+全连接队列长度      */
#define MAX_CONN        256    /* 最大并发连接数（指针数组容量）          */
#define DOCROOT         "./www"/* 默认文档根目录（可通过 -d 参数覆盖）    */
#define TIMEOUT         30     /* 连接空闲超时秒数，超时后由 check_timeout
                                  关闭连接并释放资源                     */

/* ═══════════════════════════════════════════════════════════════
   全局变量
   ═══════════════════════════════════════════════════════════════ */

extern char g_docroot[2048];   /* 文档根目录绝对路径，main.c 中由
                                  realpath(docroot) 解析填充            */

/* ═══════════════════════════════════════════════════════════════
   连接状态枚举
   ═══════════════════════════════════════════════════════════════ */

enum {
    STATE_READING,   /* 等待客户端数据到达，epoll 监听 EPOLLIN         */
    STATE_WRITING,   /* 响应数据待发送，epoll 监听 EPOLLOUT            */
    STATE_CGI        /* CGI 子进程运行中，epoll 监听管道读端 EPOLLIN   */
};

/* ═══════════════════════════════════════════════════════════════
   连接上下文 (conn_t)
   ═══════════════════════════════════════════════════════════════
   每个 TCP 客户端连接对应一个 conn_t 实例。
   实例在 handle_accept() 中通过 calloc() 堆分配，在 close_conn()
   中通过 free() 释放。采用按需分配而非静态大数组，将 BSS 段占用
   从 9.5MB 降至 2KB，同时避免 _FORTIFY_SOURCE 金丝雀误报。       */

struct conn_t {
    /* ── 基础 I/O ── */
    int   fd;                    /* 客户端 socket 文件描述符            */
    int   state;                 /* 当前状态：READING / WRITING / CGI  */

    /* ── 读缓冲区 ──
       数据从 socket recv 到 rbuf，rpos/rlen 跟踪已消费/已接收偏移。
       同时供 http_parser 增量扫描。                                  */
    char  rbuf[BUF_SIZE];        /* 接收缓冲区 (8KB)                    */
    int   rpos;                  /* 已消费偏移（parser 读取位置）       */
    int   rlen;                  /* 已接收字节总数                      */

    /* ── 写缓冲区 ──
       响应数据先写入 wbuf，再通过非阻塞 write() 分批发送。
       wpos 跟踪已发送位置，wlen 为待发送总长度。                      */
    char  wbuf[BUF_SIZE];        /* 发送缓冲区 (8KB)                    */
    int   wpos;                  /* 已发送偏移                          */
    int   wlen;                  /* 待发送总字节数                      */

    time_t last_active;          /* 最后一次 I/O 时间戳，用于超时检测   */

    /* ── HTTP 协议层 ── */
    http_parser  parser;         /* 请求解析器状态机实例                */
    http_request request;        /* 解析完成的请求结构体
                                   (method, uri, query, body...)       */

    /* ── CGI 上下文 ──
       仅在请求被路由到 exec_cgi() 时使用。
       父进程 fork() 后，将子进程 stdout 重定向到 pipe，pipe 读端
       (cgi_fd) 注册到 epoll。CGI 输出异步读取到 cgi_output。         */
    int   cgi_fd;                /* CGI 管道读端 fd，用于 epoll 注册    */
    pid_t cgi_pid;               /* CGI 子进程 PID，用于超时 kill 和
                                   reap_zombies() 回收                 */
    char  cgi_output[BUF_SIZE];  /* CGI 子进程 stdout 输出缓冲区        */
    int   cgi_olen;              /* CGI 输出实际字节数                  */

    /* ── 大文件发送上下文 (OTA等) ── */
    int   file_fd;                /* 正在发送的文件描述符，-1表示无      */
    off_t file_offset;            /* 已发送字节数                        */
    off_t file_size;              /* 文件总大小                          */
};

/* ═══════════════════════════════════════════════════════════════
   连接池 — 指针数组
   ═══════════════════════════════════════════════════════════════
   以 fd 为索引的指针数组。每个槽位为 8 字节指针，总 BSS 占用仅
   2KB (256×8)。实际 conn_t 在 accept 时按需 calloc，close 时
   free。未使用的槽位保持 NULL。                                     */

extern conn_t *g_conns[MAX_CONN];

/* ═══════════════════════════════════════════════════════════════
   服务器共享状态 (server_stats_t)
   ═══════════════════════════════════════════════════════════════
   全局单例，记录服务器运行期指标。通过 pthread_mutex_t 保护并发
   访问（日志线程 + 主事件循环）。/api/status 端点对外暴露。         */

typedef struct {
    pthread_mutex_t lock;        /* 互斥锁，保护以下字段的并发读写     */
    int     conn_count;          /* 当前活跃连接数                     */
    int     req_total;           /* 已处理请求总数                     */
    time_t  start_time;          /* 服务器启动时间戳                   */
} server_stats_t;

extern server_stats_t g_stats;   /* 全局共享统计实例                   */
extern logger_t *g_logger;       /* 全局日志句柄（logger.c 初始化）    */

/* ═══════════════════════════════════════════════════════════════
   API 函数声明
   ═══════════════════════════════════════════════════════════════ */

/* ── 网络工具 ───────────────────────────────────────────────── */

/**
 * @brief  创建非阻塞 TCP 监听 socket
 * @param  port  监听端口号 (1~65535)
 * @return 成功返回 fd，失败返回 -1
 *
 * 内部流程：socket() → setsockopt(SO_REUSEADDR) → bind() → listen()
 * → set_nonblock()。SO_REUSEADDR 允许快速重启时重用 TIME_WAIT 端口。 */
int   create_listen_socket(int port);

/**
 * @brief  将 fd 设为非阻塞模式
 * @param  fd  目标文件描述符
 * @return 0 成功，-1 失败
 *
 * 对 client socket、CGI pipe 读端、listen socket 等所有注册到 epoll
 * 的 fd 均需设为非阻塞，避免单次 I/O 调用阻塞整个事件循环。         */
int   set_nonblock(int fd);

/* ── 事件处理器 ─────────────────────────────────────────────── */

/**
 * @brief  处理新连接到达 (listen_fd 上的 EPOLLIN)
 * @param  epfd       epoll 实例 fd
 * @param  listen_fd  监听 socket fd
 *
 * 循环 accept() 直到返回 EAGAIN（一次性接纳所有就绪连接）。
 * 每个新连接：calloc conn_t → set_nonblock → epoll_ctl ADD → 更新统计。 */
void  handle_accept(int epfd, int listen_fd);

/**
 * @brief  处理客户端数据到达 (client_fd 上的 EPOLLIN)
 * @param  epfd  epoll 实例 fd
 * @param  c     连接上下文
 *
 * recv() 读取数据到 rbuf → 驱动 http_parser 状态机 → 解析完成后
 * 调用 route_request() 分发。若 recv 返回 0（对端关闭）或错误，
 * 则调用 close_conn() 释放连接。                                     */
void  handle_read(int epfd, conn_t *c);

/**
 * @brief  处理可写事件 (client_fd 上的 EPOLLOUT)
 * @param  epfd  epoll 实例 fd
 * @param  c     连接上下文
 *
 * 非阻塞 write() 将 wbuf[wpos..wlen] 分批发送。全部发送完毕后
 * 调用 close_conn()（HTTP/1.0 无 keep-alive）。                      */
void  handle_write(int epfd, conn_t *c);

/**
 * @brief  关闭连接并释放全部资源
 * @param  epfd  epoll 实例 fd
 * @param  c     连接上下文
 *
 * 执行流程：
 *   1. epoll_ctl DEL 移除 fd 监听
 *   2. 若 state == STATE_CGI：kill(cgi_pid, SIGKILL) 终止子进程
 *   3. close(fd) + close(cgi_fd)
 *   4. g_conns[fd] = NULL + free(c)
 *   5. g_stats.conn_count--                                     */
void  close_conn(int epfd, conn_t *c);

/* ── epoll 辅助 ─────────────────────────────────────────────── */

/**
 * @brief  修改连接在 epoll 中的监听事件
 * @param  epfd    epoll 实例 fd
 * @param  c       连接上下文
 * @param  events  新的事件掩码 (EPOLLIN / EPOLLOUT)
 *
 * 根据 c->state 切换监听方向：
 *   STATE_READING → EPOLLIN
 *   STATE_WRITING → EPOLLOUT
 *   STATE_CGI     → EPOLLIN (监听 CGI pipe 读端)                   */
void  mod_epoll(int epfd, conn_t *c, int events);

/* ── HTTP 响应生成 ──────────────────────────────────────────── */

/**
 * @brief  发送 HTTP 错误响应
 * @param  epfd  epoll 实例 fd
 * @param  c     连接上下文
 * @param  code  HTTP 状态码 (400/403/404/405/500)
 * @param  msg   状态描述文本
 *
 * 内置每种错误码对应的 HTML body 模板，设置 Connection: close。
 * 调用后连接进入 STATE_WRITING，由 handle_write() 发送。            */
void  send_error(int epfd, conn_t *c, int code, const char *msg);

/**
 * @brief  提供静态文件服务
 * @param  epfd     epoll 实例 fd
 * @param  c        连接上下文
 * @param  filepath 已通过 realpath() 验证的绝对路径
 * @param  st       文件 stat 信息（用于 Content-Length 和时间头）
 *
 * 构造 HTTP 200 响应：状态行 → Server/Date/Last-Modified
 * → Content-Type（根据扩展名自动检测 17 种 MIME）
 * → Content-Length → 空行 → 文件内容读入 wbuf。
 * 文件大于 wbuf 剩余空间时截断（嵌入式场景文件均较小）。             */
void  serve_file(int epfd, conn_t *c, const char *filepath, struct stat *st);

/**
 * @brief  内置 API 端点：返回服务器运行状态 JSON
 * @param  epfd  epoll 实例 fd
 * @param  c     连接上下文
 *
 * GET /api/status → {"conn_count":N, "req_total":N, "uptime":N}
 * 通过 pthread_mutex_lock 安全读取 g_stats。                         */
void  serve_api_status(int epfd, conn_t *c);

/* ── CGI 引擎 ───────────────────────────────────────────────── */

/**
 * @brief  异步执行 CGI 脚本（核心创新）
 * @param  epfd        epoll 实例 fd
 * @param  c           连接上下文（含已解析的 HTTP 请求）
 * @param  script_path CGI 脚本的绝对文件系统路径
 *
 * ## 执行流程（全异步，不阻塞事件循环）：
 *
 * ### 父进程（事件循环线程）：
 *   1. pipe(pipe_out) + pipe(pipe_in)  创建两对管道
 *   2. fork()                          创建子进程
 *   3. close(pipe_out[1])              关闭写端
 *   4. 将 POST body 写入 pipe_in → close(pipe_in)
 *   5. set_nonblock(pipe_out[0])       管道读端设为非阻塞
 *   6. epoll_ctl(ADD, pipe_out[0], EPOLLIN)
 *      ↑ 核心：管道 fd 注册到 epoll，后续 CGI 输出到达时
 *        由 handle_cgi_read() 异步读取，不阻塞主循环
 *   7. 返回事件循环，继续处理其他连接
 *
 * ### 子进程（CGI 脚本）：
 *   1. dup2(pipe_out[1], STDOUT)       stdout → 管道
 *   2. dup2(pipe_in[0], STDIN)         stdin  ← 管道（POST 场景）
 *   3. chdir(g_docroot)                切换到文档根目录
 *   4. setenv(REQUEST_METHOD, QUERY_STRING, SCRIPT_NAME,
 *             CONTENT_LENGTH, SERVER_PROTOCOL, SERVER_SOFTWARE)
 *   5. execve(script_path)             执行 CGI 脚本
 *
 * ## 与传统 CGI 的关键区别：
 *   传统方式在 fork 后阻塞 waitpid()，完全卡住工作进程。
 *   本实现通过 epoll + 管道将 CGI 输出也纳入事件驱动模型，
 *   父进程在等待 CGI 期间可继续 accept/read/write 其他连接。        */
void  exec_cgi(int epfd, conn_t *c, const char *script_path);

/* ── 请求路由 ───────────────────────────────────────────────── */

/**
 * @brief  HTTP 请求路由分发器
 * @param  epfd  epoll 实例 fd
 * @param  c     连接上下文（parser 已完成，request 已填充）
 *
 * 路由规则（按优先级）：
 *   1. /api/status        → serve_api_status()   内置 JSON 端点
 *   2. /cgi-bin/*         → exec_cgi()           异步 CGI 执行
 *   3. POST 非 CGI 路径    → 405 Method Not Allowed
 *   4. GET /*              → serve_file()         静态文件服务
 *
 * CGI 路由先通过 access(script_path, X_OK) 验证脚本存在且可执行。
 * 静态文件路由通过 realpath() + 前缀匹配防御路径穿越攻击。          */
void  route_request(int epfd, conn_t *c);

/* ── 定时维护 ───────────────────────────────────────────────── */

/**
 * @brief  检查并关闭超时连接
 * @param  epfd  epoll 实例 fd
 *
 * 遍历 g_conns 数组，对 last_active 超过 TIMEOUT(30s) 的连接
 * 调用 close_conn()。由主循环每约 10 秒触发一次。                  */
void  check_timeout(int epfd);

/**
 * @brief  回收已终止的 CGI 僵尸子进程
 *
 * while (waitpid(-1, &status, WNOHANG) > 0) 循环回收所有已终止
 * 的子进程。与 check_timeout() 同频调用（约每 10 秒）。
 *
 * 注意：WSL1 环境下 waitpid(-1,...) 可能不工作（WSL1 的
 * /proc 文件系统限制导致僵尸进程残留），但逻辑在真实 Linux
 * 内核下正确。                                                     */
void  reap_zombies(void);

/**
 * @brief  根据文件扩展名返回 MIME 类型字符串
 * @param  path  文件路径（或仅文件名）
 * @return Content-Type 字符串，如 "text/html"
 *
 * 内置映射表覆盖 17 种常见类型：html/css/js/json/png/jpg/gif/
 * ico/svg/txt/xml/pdf/mp3/mp4 + 回退 application/octet-stream。
 * 比较采用 strcasecmp，大小写不敏感。                              */
const char *get_mime_type(const char *path);

#endif
