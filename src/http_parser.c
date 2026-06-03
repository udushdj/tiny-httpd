#include <strings.h>
#include "http_parser.h"
#include "server.h"

/*
 * 解析请求行: "GET /path?key=val HTTP/1.1"
 * 提取 method、uri、query、version
 */
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

    char *qmark = strchr(uri_raw, '?');
    if (qmark) {
        *qmark = '\0';
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

/*
 * 解析请求头: "Host: 127.0.0.1"
 * 只提取 Content-Length，其余头部不存储
 */
static void parse_header_line(conn_t *c)
{
    char *line = c->parser.line_buf;

    if (strncasecmp(line, "Content-Length:", 15) == 0) {
        char *val = line + 15;
        while (*val == ' ' || *val == '\t') val++;
        c->parser.content_len = atoi(val);
    }
}

/*
 * 驱动 HTTP 解析状态机，从 rbuf 逐字节处理
 * 返回 0 表示解析完成，1 表示还需要更多数据
 */
int parse_request(conn_t *c)
{
    while (c->rpos < c->rlen) {
        char ch = c->rbuf[c->rpos++];

        switch (c->parser.state) {

        case PARSE_REQ_LINE:
            if (ch == '\r') continue;
            if (ch == '\n') {
                c->parser.line_buf[c->parser.line_pos] = '\0';
                parse_req_line(c);
                c->parser.state    = PARSE_HEADERS;
                c->parser.line_pos = 0;
            } else if (c->parser.line_pos < LINE_BUF_SIZE - 1) {
                c->parser.line_buf[c->parser.line_pos++] = ch;
            }
            break;

        case PARSE_HEADERS:
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (c->parser.line_pos == 0) {
                    if (c->parser.content_len > 0) {
                        c->parser.state      = PARSE_BODY;
                        c->parser.body_read  = 0;
                        c->parser.line_pos   = 0;
                    } else {
                        c->parser.state = PARSE_DONE;
                    }
                    return 0;
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
            int remaining  = c->parser.content_len - c->parser.body_read;
            int available  = c->rlen - c->rpos + 1;
            int to_read    = remaining < available ? remaining : available;

            c->parser.body_read += to_read;
            c->rpos             += to_read - 1;

            if (c->parser.body_read >= c->parser.content_len) {
                c->parser.state = PARSE_DONE;
                return 0;
            }
            break;
        }

        case PARSE_DONE:
            return 0;
        }
    }
    return 1;
}
