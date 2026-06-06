#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/**
 * @file    http_parser.h
 * @brief   HTTP/1.1 请求解析器 — 基于显式状态机的增量解析
 *
 * ## 设计理念
 *
 * HTTP 请求通过 TCP 流式传输，单次 recv() 可能只收到半个请求行或
 * 半个头部。传统做法是缓存完整请求再 sscanf/strtok 全量解析，缺点：
 *   1. 需要大缓冲区暂存整个请求
 *   2. 无法在数据到达过程中"边收边解析"
 *   3. POST body 处理与头部解析耦合
 *
 * 本解析器采用"逐字节推动 + 状态记忆"策略：
 *   - 每收到一个字符立刻送入状态机，消费完不额外缓存
 *   - 状态机记住当前位置（请求行→头部→body），碎片数据到达后
 *     从中断处继续
 *   - POST body 按 Content-Length 精确跳过，body 内容留在 rbuf
 *     供 CGI 引擎后续使用
 *
 * ## 状态转移
 *
 *   accept 时: http_parser_init() → PARSE_REQ_LINE
 *
 *   PARSE_REQ_LINE  ──► PARSE_HEADERS ──► PARSE_BODY ──► PARSE_DONE
 *        │                   │                │              │
 *    逐字节扫 \n         逐行解析头部      按 CL 跳字节    route_request()
 *    strtok_r 拆 method  遇空行(\n\n)      累加到 CL 值   分发处理
 *    /uri/version        有 CL → BODY
 *                        无 CL → DONE
 */

#define LINE_BUF_SIZE   2048        /* 单行最大字节（请求行/头部行），超出截断 */

/* ════════════════════════════════════════════════════════
   解析器状态枚举
   ════════════════════════════════════════════════════════ */

enum {
    PARSE_REQ_LINE,     /* 解析请求行: "GET /path HTTP/1.1\r\n"
                           逐字扫描到 \n → parse_req_line() 用 strtok_r
                           拆出 method、uri_raw、version → 进入 HEADERS    */

    PARSE_HEADERS,      /* 逐行解析头部: "Host: ...\r\n"
                           每遇 \n 调用 parse_header_line() 提取 Content-Length
                           遇空行（连续 \n\n）时：
                           - content_len > 0 → 切到 PARSE_BODY
                           - content_len = 0 → 直接 PARSE_DONE            */

    PARSE_BODY,         /* 按 Content-Length 跳过 POST body
                           不解析 body 内容,只累加 body_read 直到满足 CL
                           完成后进入 PARSE_DONE                          */

    PARSE_DONE          /* 解析完成，连接上下文中的 request 已填充完毕
                           调用 route_request() 根据 method+uri 分发      */
};

/* ════════════════════════════════════════════════════════
   HTTP 方法枚举
   ════════════════════════════════════════════════════════ */

enum {
    HTTP_GET,           /* GET  — 读取静态文件 / API 查询                  */
    HTTP_POST,          /* POST — 提交数据到 CGI / 写入文件                */
    HTTP_UNKNOWN        /* 非 GET/POST — route_request() 返回 405          */
};

/* ════════════════════════════════════════════════════════
   HTTP 解析器上下文
   ════════════════════════════════════════════════════════
   每个客户端连接内嵌一个实例（conn_t.parser），生命周期与连接相同。
   accept 时 http_parser_init() 归零，close_conn 时随 conn_t free。 */

typedef struct {
    int   state;                    /* 当前阶段：REQ_LINE→HEADERS→BODY→DONE */
    int   content_len;              /* POST body 字节数（从 Content-Length 提取） */
    int   body_read;                /* 已跳过的 body 字节数，与 content_len 比较 */
    char  line_buf[LINE_BUF_SIZE];  /* 当前行缓冲区：逐字填充，遇 \n 时触发处理
                                       处理完后 line_pos 归零，重复使用         */
    int   line_pos;                 /* line_buf 当前写入位置                    */
} http_parser;

/* ════════════════════════════════════════════════════════
   解析完成的 HTTP 请求
   ════════════════════════════════════════════════════════
   parse_request() 返回 0 后，conn_t.request 中包含完整的
   请求信息。route_request() 根据 method + uri 决定处理方式。  */

typedef struct {
    int   method;                   /* HTTP_GET / HTTP_POST                 */
    char  uri[1024];                /* URI 路径（已去掉 ? 及 query string） */
    char  query[1024];              /* ? 后的键值对，如 "key=val&foo=bar"   */
    char  version[16];              /* HTTP 版本，如 "HTTP/1.1"             */
    int   header_cnt;               /* 解析到的头部数量（预留扩展）          */
} http_request;

/*
 * 初始化解析器 — accept 新连接时调用，将状态机重置到起点
 */
static inline void http_parser_init(http_parser *p)
{
    p->state       = PARSE_REQ_LINE;
    p->content_len = 0;
    p->body_read   = 0;
    p->line_pos    = 0;
}

/* 前向声明：conn_t 定义在 server.h */
typedef struct conn_t conn_t;

/**
 * @brief  驱动 HTTP 解析状态机（核心入口）
 * @param  c  连接上下文，内含 rbuf + parser 实例
 * @return 0 = 解析完成，可调用 route_request() 分发
 *         1 = 数据不足，等待下次 EPOLLIN 后继续
 *
 * 函数在 handle_read() 每次收到新数据后被调用。通过 c->rpos/c->rlen
 * 跟踪 rbuf 中"已消费/已接收"的边界，逐字节推动状态机。当前数据不够时
 * 返回 1，状态机保持现场，下次 EPOLLIN 触发时从中断处继续——这就是
 * 状态机的核心价值：天然支持 TCP 碎片化数据。                    */
int parse_request(conn_t *c);

#endif
