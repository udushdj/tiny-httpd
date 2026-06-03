#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#define LINE_BUF_SIZE   2048

/* 解析器状态 */
enum {
    PARSE_REQ_LINE,     /* 解析请求行 */
    PARSE_HEADERS,      /* 解析头部 */
    PARSE_BODY,         /* 解析请求体 */
    PARSE_DONE          /* 解析完成 */
};

/* HTTP 方法 */
enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_UNKNOWN
};

/* HTTP 解析器上下文 */
typedef struct {
    int   state;                /* 当前解析状态 */
    int   content_len;          /* Content-Length 值 */
    int   body_read;            /* 已读取的 body 字节数 */
    char  line_buf[LINE_BUF_SIZE]; /* 行解析临时缓冲 */
    int   line_pos;             /* 当前行写入位置 */
} http_parser;

/* 解析后的 HTTP 请求 */
typedef struct {
    int   method;               /* HTTP_GET / HTTP_POST */
    char  uri[1024];            /* URI 路径（不含查询参数） */
    char  query[1024];          /* 查询参数（? 之后的部分） */
    char  version[16];          /* HTTP 版本 */
    int   header_cnt;           /* 头部数量 */
} http_request;

/*
 * 初始化解析器（accept 新连接时调用）
 */
static inline void http_parser_init(http_parser *p)
{
    p->state       = PARSE_REQ_LINE;
    p->content_len = 0;
    p->body_read   = 0;
    p->line_pos    = 0;
}

/* 前向声明，定义在 server.h */
typedef struct conn_t conn_t;

/*
 * 驱动 HTTP 解析状态机，从 rbuf 逐字节处理
 * 返回 0 = 解析完成，1 = 还需要更多数据
 */
int parse_request(conn_t *c);

#endif
