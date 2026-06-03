#define _GNU_SOURCE
#include <strings.h>
#include "server.h"

conn_t *g_conns[MAX_CONN];

int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) { perror("fcntl F_GETFL"); return -1; }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK"); return -1;
    }
    return 0;
}

const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".htm")  == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js")   == 0) return "application/javascript";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".png")  == 0) return "image/png";
    if (strcasecmp(ext, ".jpg")  == 0) return "image/jpeg";
    if (strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif")  == 0) return "image/gif";
    if (strcasecmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcasecmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcasecmp(ext, ".txt")  == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".xml")  == 0) return "application/xml";
    if (strcasecmp(ext, ".pdf")  == 0) return "application/pdf";
    if (strcasecmp(ext, ".mp3")  == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".mp4")  == 0) return "video/mp4";
    return "application/octet-stream";
}

int create_listen_socket(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); close(listen_fd); return -1;
    }
    if (listen(listen_fd, LISTEN_BACKLOG) == -1) {
        perror("listen"); close(listen_fd); return -1;
    }
    if (set_nonblock(listen_fd) == -1) {
        close(listen_fd); return -1;
    }
    return listen_fd;
}

void mod_epoll(int epfd, conn_t *c, int events)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1)
        perror("epoll_ctl MOD");
}

void send_error(int epfd, conn_t *c, int code, const char *msg)
{
    const char *body_fmt =
        "<!DOCTYPE html>"
        "<html><head><meta charset=\"utf-8\">"
        "<title>%d %s</title></head>"
        "<body><h1>%d %s</h1></body></html>";
    char body[512];
    int  body_len = snprintf(body, sizeof(body), body_fmt, code, msg, code, msg);

    c->wlen = snprintf(c->wbuf, BUF_SIZE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Server: tiny-httpd/0.3\r\n"
        "\r\n%s", code, msg, body_len, body);
    if (c->wlen < 0 || c->wlen >= BUF_SIZE) c->wlen = BUF_SIZE - 1;

    c->wpos = 0;
    c->state = STATE_WRITING;
    mod_epoll(epfd, c, EPOLLOUT);
}

void serve_file(int epfd, conn_t *c, const char *filepath, struct stat *st)
{
    int fd = open(filepath, O_RDONLY);
    if (fd == -1) { send_error(epfd, c, 404, "Not Found"); return; }
    const char *mime = get_mime_type(filepath);

    int hlen = snprintf(c->wbuf, BUF_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Server: tiny-httpd/0.3\r\n"
        "\r\n", mime, (long)st->st_size);
    if (hlen < 0 || hlen >= BUF_SIZE) {
        close(fd); send_error(epfd, c, 500, "Internal Server Error"); return;
    }

    int remaining = BUF_SIZE - hlen;
    int to_read   = (st->st_size < remaining) ? (int)st->st_size : remaining;
    int nread     = read(fd, c->wbuf + hlen, to_read);
    close(fd);

    if (nread < 0) { send_error(epfd, c, 500, "Internal Server Error"); return; }

    c->wlen = hlen + nread;
    c->wpos = 0;
    c->state = STATE_WRITING;
    mod_epoll(epfd, c, EPOLLOUT);

    printf("[INFO] %d %s -> 200 %s (%ld bytes)\n",
           c->fd, c->request.uri, mime, (long)st->st_size);
}

/* ===================== CGI 引擎 ===================== */

void exec_cgi(int epfd, conn_t *c, const char *script_path)
{
    int pipe_out[2];
    if (pipe(pipe_out) == -1) {
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipe_out[0]); close(pipe_out[1]);
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    if (pid == 0) {
        /* 子进程：关闭不需要的描述符 */
        close(pipe_out[0]);

        /* 关闭除 stdout 外的 stdin/stderr，避免干扰 */
        close(STDIN_FILENO);
        close(STDERR_FILENO);

        if (dup2(pipe_out[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }
        close(pipe_out[1]);

        if (chdir(g_docroot) == -1) {
            _exit(127);
        }

        setenv("REQUEST_METHOD",  "GET", 1);
        setenv("QUERY_STRING",    c->request.query[0] ? c->request.query : "", 1);
        setenv("SCRIPT_NAME",     c->request.uri, 1);
        setenv("CONTENT_LENGTH",  "0", 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        setenv("SERVER_SOFTWARE", "tiny-httpd/0.3", 1);

        extern char **environ;
        char *argv[] = { (char *)script_path, NULL };
        execve(script_path, argv, environ);
        /* execve 失败时才走到这里 */
        _exit(127);
    }

    /* 父进程 */
    close(pipe_out[1]);

    /* 阻塞读管道 */
    char cgi_output[BUF_SIZE];
    int  total = 0;
    while (total < BUF_SIZE - 1) {
        ssize_t n = read(pipe_out[0], cgi_output + total, BUF_SIZE - total - 1);
        if (n > 0) {
            total += n;
        } else if (n == 0) {
            break;  /* EOF：子进程关闭了写端 */
        } else {
            if (errno == EINTR) continue;
            break;  /* 错误 */
        }
    }
    cgi_output[total] = '\0';
    close(pipe_out[0]);
    waitpid(pid, NULL, 0);

    printf("[INFO] %d CGI done pid=%d, %d bytes\n", c->fd, pid, total);
    fflush(stdout);

    if (total == 0) {
        /* CGI 无输出，返回 500 */
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    /* 解析 \r\n\r\n 分隔头/体 */
    char *header_end = strstr(cgi_output, "\r\n\r\n");
    char *body;
    int   body_len;
    const char *content_type = "text/html; charset=utf-8";

    if (header_end) {
        *header_end = '\0';
        body     = header_end + 4;
        body_len = total - (int)(body - cgi_output);

        char *ct = strcasestr(cgi_output, "Content-Type:");
        if (ct) {
            ct += 13;
            while (*ct == ' ' || *ct == '\t') ct++;
            char *end = ct;
            while (*end && *end != '\r' && *end != '\n') end++;
            int ctlen = (int)(end - ct);
            if (ctlen > 0 && ctlen < 128) {
                static char ct_buf[128];
                memcpy(ct_buf, ct, ctlen);
                ct_buf[ctlen] = '\0';
                content_type = ct_buf;
            }
        }
    } else {
        body     = cgi_output;
        body_len = total;
    }
    if (body_len < 0) body_len = 0;

    /* 构造 HTTP 响应头 + CGI body */
    int hlen = snprintf(c->wbuf, BUF_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Server: tiny-httpd/0.3\r\n"
        "\r\n", content_type, body_len);
    if (hlen < 0 || hlen >= BUF_SIZE) { send_error(epfd, c, 500, "Internal Server Error"); return; }

    int to_copy = body_len;
    if (hlen + to_copy > BUF_SIZE) to_copy = BUF_SIZE - hlen;
    memcpy(c->wbuf + hlen, body, to_copy);

    c->wlen  = hlen + to_copy;
    c->wpos  = 0;
    c->state = STATE_WRITING;
    mod_epoll(epfd, c, EPOLLOUT);
}

/* ===================== 路由 ===================== */

void route_request(int epfd, conn_t *c)
{
    char uri[1024];

    if (strcmp(c->request.uri, "/") == 0)
        strcpy(uri, "/index.html");
    else {
        strncpy(uri, c->request.uri, sizeof(uri) - 1);
        uri[sizeof(uri) - 1] = '\0';
    }

    /* CGI 路由：/cgi-bin/ 前缀 */
    if (strncmp(uri, "/cgi-bin/", 9) == 0) {
        if (c->request.method != HTTP_GET) {
            send_error(epfd, c, 405, "Method Not Allowed");
            return;
        }
        char script_path[2048];
        snprintf(script_path, sizeof(script_path), "%s%s", g_docroot, uri);
        exec_cgi(epfd, c, script_path);
        return;
    }

    if (c->request.method != HTTP_GET) {
        send_error(epfd, c, 405, "Method Not Allowed");
        return;
    }

    char full_path[2048];
    snprintf(full_path, sizeof(full_path), "%s%s", g_docroot, uri);

    char resolved[2048];
    if (realpath(full_path, resolved) == NULL) {
        send_error(epfd, c, 404, "Not Found");
        return;
    }

    if (strncmp(resolved, g_docroot, strlen(g_docroot)) != 0) {
        send_error(epfd, c, 403, "Forbidden");
        return;
    }

    struct stat st;
    if (stat(resolved, &st) == -1 || !S_ISREG(st.st_mode)) {
        send_error(epfd, c, 404, "Not Found");
        return;
    }

    serve_file(epfd, c, resolved, &st);
}

/* ===================== 连接生命周期 ===================== */

void handle_accept(int epfd, int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);

    while (1) {
        int fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept"); break;
        }
        if (fd >= MAX_CONN) { close(fd); continue; }
        if (set_nonblock(fd) == -1) { close(fd); continue; }

        conn_t *c = calloc(1, sizeof(conn_t));
        if (!c) { close(fd); continue; }
        c->fd    = fd;
        c->state = STATE_READING;
        http_parser_init(&c->parser);
        g_conns[fd] = c;

        struct epoll_event ev;
        ev.events   = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl ADD"); close(fd); continue;
        }
        printf("[INFO] 新连接: fd=%d, %s:%d\n",
               fd, inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));
    }
}

void handle_read(int epfd, conn_t *c)
{
    int n = read(c->fd, c->rbuf + c->rlen, BUF_SIZE - c->rlen);

    if (n == 0) { close_conn(epfd, c); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        perror("read"); close_conn(epfd, c); return;
    }
    c->rlen += n;

    if (parse_request(c) == 0)
        route_request(epfd, c);
}

void handle_write(int epfd, conn_t *c)
{
    int n = write(c->fd, c->wbuf + c->wpos, c->wlen - c->wpos);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        perror("write"); close_conn(epfd, c); return;
    }
    c->wpos += n;
    if (c->wpos >= c->wlen) close_conn(epfd, c);
}

void close_conn(int epfd, conn_t *c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    g_conns[c->fd] = NULL;
    free(c);
}
