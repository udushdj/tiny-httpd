#include <strings.h>
#include "http_parser.h"
#include "server.h"

/* ════════════════════════════════════════════════════════
   请求行解析器
   ════════════════════════════════════════════════════════
   输入: line_buf 中的一行，如 "GET /cgi-bin/api?key=val HTTP/1.1"
   用 strtok_r (线程安全版 strtok) 按空格拆分三个 token：
     method  → 映射到 HTTP_GET / HTTP_POST / HTTP_UNKNOWN
     uri_raw → 按 '?' 拆分为 uri (路径) 和 query (查询参数)
     version → 存入 request.version（当前未使用，预留）
   若格式异常（无 method 或 uri），method 置为 HTTP_UNKNOWN
   后续路由会返回 400 Bad Request。                              */
static void parse_req_line(conn_t *c)
{
    char *line = c->parser.line_buf;
    char *saveptr;
    char *method  = strtok_r(line,  " ", &saveptr);
    char *uri_raw = strtok_r(NULL, " ", &saveptr);
    char *version = strtok_r(NULL, " ", &saveptr);

    if (!method || !uri_raw) {
        c->request.method = HTTP_UNKNOWN;
        return;
    }

    if (strcasecmp(method, "GET") == 0) {
        c->request.method = HTTP_GET;
    } else if (strcasecmp(method, "POST") == 0) {
        c->request.method = HTTP_POST;
    } else {
        c->request.method = HTTP_UNKNOWN;
    }

    /* 拆分 URI 和 query string：
       "/cgi-bin/api?target=stats&foo=bar"
           ↓
       uri="/cgi-bin/api"   query="target=stats&foo=bar"         */
    char *qmark = strchr(uri_raw, '?');
    if (qmark) {
        *qmark = '\0';   /* 在 '?' 处截断 uri_raw，将原串切成两段 */
        strncpy(c->request.uri, uri_raw, sizeof(c->request.uri) - 1);
        strncpy(c->request.query, qmark + 1, sizeof(c->request.query) - 1);
    } else {
        strncpy(c->request.uri, uri_raw, sizeof(c->request.uri) - 1);
        c->request.query[0] = '\0';
    }

    if (version) {
        strncpy(c->request.version, version, sizeof(c->request.version) - 1);
    }
}

/* ════════════════════════════════════════════════════════
   头部行解析器
   ════════════════════════════════════════════════════════
   输入: line_buf 中的一行头部，如 "Content-Length: 389"
   设计决策：当前仅提取 Content-Length（POST body 长度必备）。
   其他头部（Host, User-Agent, Cookie 等）被忽略。
   如需扩展（如虚拟主机支持），在此处添加 strncasecmp 分支即可。 */
static void parse_header_line(conn_t *c)
{
    char *line = c->parser.line_buf;

    if (strncasecmp(line, "Content-Length:", 15) == 0) {
        char *val = line + 15;
        while (*val == ' ' || *val == '\t') val++;   /* 跳过冒号后空白 */
        c->parser.content_len = atoi(val);
    }
}

/* ════════════════════════════════════════════════════════
   HTTP 解析状态机 — 主驱动函数
   ════════════════════════════════════════════════════════
   每次 handle_read() 收到新数据后调用本函数。
   逐字节消费 rbuf[c->rpos .. c->rlen]，通过 switch(c->parser.state)
   进入对应阶段处理，直到数据耗尽或状态机到达 PARSE_DONE。

   ## 各状态关键逻辑：

   PARSE_REQ_LINE:
       逐字读到 \n → line_buf 封底 → parse_req_line()
       拆分 method/uri/version → 切换 PARSE_HEADERS
       注意：\r 直接跳过（HTTP 行尾是 \r\n，只认 \n）

   PARSE_HEADERS:
       逐字读到 \n：
       - 若 line_pos==0（空行 = 连续的 \n\n）→ 头部结束
         · 有 Content-Length → 切 PARSE_BODY
         · 无 Content-Length → 直接 PARSE_DONE
       - 若 line_pos>0                → parse_header_line()
         提取 Content-Length → line_pos 归零继续下行

   PARSE_BODY:
       批量跳过字节（不逐字扫描，效率优化）：
       - 计算剩余需要字节数 remaining = content_len - body_read
       - 计算 rbuf 中可用字节数 available
       - 一次跳过 min(remaining, available) 字节
       - 若跳够 content_len → PARSE_DONE
       body 内容留在 rbuf 中（通过 rpos 标记），供 CGI 引擎使用

   返回 0 → PARSE_DONE → route_request() 分发处理
   返回 1 → 数据不够 → 等待下次 EPOLLIN 继续，状态机保持现场     */
int parse_request(conn_t *c)
{
    while (c->rpos < c->rlen) {
        char ch = c->rbuf[c->rpos++];

        switch (c->parser.state) {

        case PARSE_REQ_LINE:
            if (ch == '\r') continue;   /* 跳过 \r，只等 \n */
            if (ch == '\n') {
                c->parser.line_buf[c->parser.line_pos] = '\0';
                parse_req_line(c);
                c->parser.state    = PARSE_HEADERS;
                c->parser.line_pos = 0;   /* 归零，准备存下一行头部 */
            } else if (c->parser.line_pos < LINE_BUF_SIZE - 1) {
                c->parser.line_buf[c->parser.line_pos++] = ch;
            }
            break;

        case PARSE_HEADERS:
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (c->parser.line_pos == 0) {
                    /* 空行 = 头部结束 */
                    if (c->parser.content_len > 0) {
                        c->parser.state      = PARSE_BODY;
                        c->parser.body_read  = 0;
                        c->parser.line_pos   = 0;
                    } else {
                        c->parser.state = PARSE_DONE;
                    }
                    return 0;   /* GET 请求在此完成 */
                } else {
                    c->parser.line_buf[c->parser.line_pos] = '\0';
                    parse_header_line(c);
                    c->parser.line_pos = 0;
                }
            } else if (c->parser.line_pos < LINE_BUF_SIZE - 1) {
                c->parser.line_buf[c->parser.line_pos++] = ch;
            }
            break;

        case PARSE_BODY: {
            /* 批量跳过：计算剩余和可用，取最小值一次性消费 */
            int remaining  = c->parser.content_len - c->parser.body_read;
            int available  = c->rlen - c->rpos + 1;
            int to_read    = remaining < available ? remaining : available;

            c->parser.body_read += to_read;
            c->rpos             += to_read - 1;   /* -1 抵消外层 rpos++ */

            if (c->parser.body_read >= c->parser.content_len) {
                c->parser.state = PARSE_DONE;
                return 0;   /* POST 请求在此完成 */
            }
            break;
        }

        case PARSE_DONE:
            return 0;
        }
    }

    return 1;   /* 数据不够，等待下次 EPOLLIN */
}
