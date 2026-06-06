#define _GNU_SOURCE
#include <strings.h>
#include "server.h"

conn_t *g_conns[MAX_CONN];
server_stats_t g_stats;
logger_t *g_logger;

/*
    --设置非阻塞模式
    --fd:   指定的文件描述符

    非阻塞是 epoll 边缘触发 (EPOLLET) 的前提，
    也是单线程事件循环不卡死的关键。
    所有注册到 epoll 的 fd（listen/client/CGI pipe）都需设置此标志。 */
int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) { perror("fcntl F_GETFL"); return -1; }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK"); return -1;
    }
    return 0;
}

/*
 * 根据文件扩展名匹配 MIME 类型
 * 内置 17 种常见类型映射表，回退为 application/octet-stream
 * 大小写不敏感 (strcasecmp)
 */
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

/*
 * 修改连接在 epoll 中的监听事件
 * 根据 c->state 切换监听方向：READING→EPOLLIN, WRITING→EPOLLOUT
 * 特殊处理：当 EPOLL_CTL_MOD 返回 ENOENT（fd 不在 epoll 中，
 * 如 CGI 完成后恢复客户端 fd 时），退化为 EPOLL_CTL_ADD 重新注册
 */
void mod_epoll(int epfd, conn_t *c, int events)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1 && errno == ENOENT) {
        /* fd 不在 epoll 中（如 CGI 完成后），重新添加 */
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev) == -1)
            perror("epoll_ctl ADD");
    }
}

/*
 * 构造 HTTP 错误响应
 * 内置 HTML 模板（含中文 UTF-8 编码声明），支持 400/403/404/405/500。
 * 响应写入 wbuf 后连接切换到 STATE_WRITING，由 handle_write() 发送。
 */
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
        "Server: tiny-httpd/0.4\r\n"
        "\r\n%s", code, msg, body_len, body);
    if (c->wlen < 0 || c->wlen >= BUF_SIZE) c->wlen = BUF_SIZE - 1;

    c->wpos = 0;
    c->state = STATE_WRITING;
    mod_epoll(epfd, c, EPOLLOUT);
}

/*
 * 静态文件服务
 * 打开文件 → 构造 HTTP 200 响应头 (含 MIME/Content-Length) →
 * 读取文件内容到 wbuf → 切到 STATE_WRITING 发送。
 * 文件大于 wbuf 剩余空间时截断（嵌入式场景文件均很小）。
 */
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
        "Server: tiny-httpd/0.4\r\n"
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

    log_access(g_logger, "127.0.0.1", "GET", c->request.uri, 200, 0);
    printf("[INFO] %d %s -> 200 %s (%ld bytes)\n",
           c->fd, c->request.uri, mime, (long)st->st_size);
}

/*
 * 内置 API 端点: GET /api/status
 * 返回服务器运行状态 JSON: {"conn_count":N, "req_total":N, "uptime":N}
 * 通过 pthread_mutex_lock 安全读取全局统计。
 */
void serve_api_status(int epfd, conn_t *c)
{
    pthread_mutex_lock(&g_stats.lock);
    time_t uptime = time(NULL) - g_stats.start_time;
    int conn = g_stats.conn_count;
    int req  = g_stats.req_total;
    pthread_mutex_unlock(&g_stats.lock);

    char json[256];
    int len = snprintf(json, sizeof(json),
        "{\"conn_count\":%d,\"req_total\":%d,\"uptime\":%ld}",
        conn, req, (long)uptime);

    c->wlen = snprintf(c->wbuf, BUF_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Server: tiny-httpd/0.4\r\n"
        "\r\n%s", len, json);
    if (c->wlen < 0 || c->wlen >= BUF_SIZE) {
        send_error(epfd, c, 500, "Internal Server Error"); return;
    }

    c->wpos = 0;
    c->state = STATE_WRITING;
    mod_epoll(epfd, c, EPOLLOUT);

    log_access(g_logger, "127.0.0.1", "GET", "/api/status", 200, 0);
    printf("[INFO] %d /api/status -> 200 JSON\n", c->fd);
}

/* ===================== CGI 引擎 ===================== */

/* 
    --  将 pipe 数据读入缓冲区，检测到 EOF 或错误后解析 CGI 输出并构建 HTTP 响应。
    --  epfd：  epoll实例
    --  c:      conn_t结构体指针（承载请求信息）
*/
void handle_cgi_read(int epfd, conn_t *c)
{
    int pipe_fd = c->cgi_fd;           // CGI 输出管道的读取端 fd
    char *buf   = c->cgi_output;       // 累积输出的缓冲区
    int  *olen  = &c->cgi_olen;        // 当前已读取的字节数

    /*
     * 尝试读取管道数据，保留一个字节给结尾 '\0'。
     * 注意：这里没有检查 *olen 是否已超过 BUF_SIZE，存在缓冲区溢出风险，
     * 实际使用中应确保 CGI 输出总长度不超过 BUF_SIZE - 1。
     */
    ssize_t n = read(pipe_fd, buf + *olen, BUF_SIZE - *olen - 1);
    if (n > 0) {
        *olen += n;
        return;  /* 数据尚未读完，继续等待下一次可读事件 */
    }

    /* n <= 0：管道关闭（EOF）或读取错误，无论哪种情况都结束 CGI 处理 */
    close(pipe_fd);                         // 关闭管道读取端
    waitpid(c->cgi_pid, NULL, 0);           // 回收 CGI 子进程，避免僵尸进程
    /* 注意：waitpid 可能阻塞，若 CGI 进程未结束会占用事件循环线程 */

    int total = *olen;
    buf[total] = '\0';                      // 确保字符串结尾

    printf("[INFO] %d CGI done pid=%d, %d bytes\n", c->fd, c->cgi_pid, total);
    fflush(stdout);

    /* CGI 无任何输出，返回 500 错误 */
    if (total == 0) {
        const char *body = "Internal Server Error";
        c->wlen = snprintf(c->wbuf, BUF_SIZE,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "Server: tiny-httpd/0.4\r\n"
            "\r\n%s", (long)strlen(body), body);
        c->wpos  = 0;
        c->state = STATE_WRITING;
        mod_epoll(epfd, c, EPOLLOUT);       // 切换到写状态，发送响应
        return;
    }

    /*
     * CGI 有输出，需要解析其原始的 HTTP 头（CGI 输出的标准形式是先头后体，
     * 以 \r\n\r\n 分隔）。如果找到头部结束标志，尝试提取 Content-Type 字段；
     * 否则整个输出都当作响应体。
     */
    char *header_end = strstr(buf, "\r\n\r\n");
    char *body;
    int   body_len;
    const char *content_type = "text/html; charset=utf-8";  // 默认类型

    if (header_end) {
        *header_end = '\0';              // 临时截断，使 buf 指向头部部分
        body     = header_end + 4;       // 跳过 \r\n\r\n
        body_len = total - (int)(body - buf);

        /*
         * 在头部中查找 Content-Type 行。
         * 注意：strcasestr 非标准 C，某些环境需用 _stricmp 或自定义实现。
         * 解析方式较简陋：假设 "Content-Type:" 后紧跟值，支持前导空白。
         */
        char *ct = strcasestr(buf, "Content-Type:");
        if (ct) {
            ct += 13;                    // 跳过 "Content-Type:"
            while (*ct == ' ' || *ct == '\t') ct++;  // 跳过前导空白
            char *end = ct;
            while (*end && *end != '\r' && *end != '\n') end++;  // 寻找行尾
            int ctlen = (int)(end - ct);
            if (ctlen > 0 && ctlen < 128) {
                /*
                 * 使用静态缓冲区临时存储内容类型。
                 * 非线程安全！多线程环境需改为每连接分配或使用锁。
                 */
                char ct_buf[128];
                memcpy(ct_buf, ct, ctlen);
                ct_buf[ctlen] = '\0';
                content_type = ct_buf;
            }
        }
    } else {
        /* 没有找到头部结束标志，整个 CGI 输出视为响应体 */
        body     = buf;
        body_len = total;
    }
    if (body_len < 0) body_len = 0;       // 安全保护

    /* 构造最终的 HTTP 响应头部 */
    int hlen = snprintf(c->wbuf, BUF_SIZE,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Server: tiny-httpd/0.4\r\n"
        "\r\n", content_type, body_len);
    if (hlen < 0 || hlen >= BUF_SIZE) {
        /* 头部构造失败（如 BUF_SIZE 过小），回退发送 500 错误 */
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    /*
     * 将响应体拼接到头部之后，但要防止写入越界。
     * 如果 BUF_SIZE 不足以容纳完整的头部和体，则截断。
     */
    int to_copy = body_len;
    if (hlen + to_copy > BUF_SIZE) to_copy = BUF_SIZE - hlen;
    memcpy(c->wbuf + hlen, body, to_copy);

    c->wlen  = hlen + to_copy;   // 待发送的总字节数
    c->wpos  = 0;                // 已发送字节数清零
    c->state = STATE_WRITING;    // 切换到写状态
    mod_epoll(epfd, c, EPOLLOUT); // 修改 epoll 监听事件为 EPOLLOUT
}

/*
 * 异步 CGI 执行器 (核心创新)
 *
 * ## 为什么"异步"？
 *
 * 传统 CGI (Apache mod_cgi) 在 fork 后阻塞 waitpid()，完全卡住工作
 * 进程/线程。在单线程 epoll 架构中这不可接受——一个慢 CGI 会拖死全服。
 *
 * 本实现通过 epoll + 管道将 CGI 输出也纳入事件驱动模型：
 *   1. fork 后子进程 stdout 重定向到 pipe
 *   2. 父进程关闭 pipe 写端，将读端设为非阻塞并注册到 epoll
 *   3. 父进程立即返回事件循环，继续处理其他连接
 *   4. CGI 输出到达时 epoll 通知 → handle_cgi_read() 异步读取
 *   5. zombie 由定时器 reap_zombies() 定期回收
 *
 * ## 执行流程
 *
 * 父进程 (事件循环线程):
 *   pipe(pipe_out) + pipe(pipe_in) → fork()
 *   ├─ close(pipe_out[1])           关闭写端，只读
 *   ├─ 将 POST body 写入 pipe_in    输入数据给子进程
 *   ├─ set_nonblock(pipe_out[0])    管道读端非阻塞
 *   ├─ epoll_ctl(ADD, pipe_out[0])  ★ 核心：管道 fd 注册到 epoll
 *   └─ return                       立即返回，不阻塞
 *
 * 子进程 (CGI 脚本):
 *   ├─ dup2(pipe_out[1], STDOUT)    stdout → 管道写端
 *   ├─ dup2(pipe_in[0], STDIN)      stdin  ← POST body
 *   ├─ chdir(g_docroot)             切换到文档根目录
 *   ├─ setenv(REQUEST_METHOD, QUERY_STRING, ...)
 *   └─ execve(script_path)          执行脚本
 */
void exec_cgi(int epfd, conn_t *c, const char *script_path)
{
    int pipe_out[2], pipe_in[2] = {-1, -1};
    int has_stdin = (c->request.method == HTTP_POST && c->parser.content_len > 0);

    if (pipe(pipe_out) == -1) {
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    if (has_stdin && pipe(pipe_in) == -1) {
        close(pipe_out[0]); close(pipe_out[1]);
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipe_out[0]); close(pipe_out[1]);
        if (has_stdin) { close(pipe_in[0]); close(pipe_in[1]); }
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    if (pid == 0) {
        close(pipe_out[0]);
        close(STDIN_FILENO);
        close(STDERR_FILENO);

        if (dup2(pipe_out[1], STDOUT_FILENO) == -1) _exit(127);
        close(pipe_out[1]);

        if (has_stdin) {
            if (dup2(pipe_in[0], STDIN_FILENO) == -1) _exit(127);
            close(pipe_in[0]); close(pipe_in[1]);
        }

        if (chdir(g_docroot) == -1) _exit(127);

        setenv("REQUEST_METHOD",  c->request.method == HTTP_POST ? "POST" : "GET", 1);
        setenv("QUERY_STRING",    c->request.query[0] ? c->request.query : "", 1);
        setenv("SCRIPT_NAME",     c->request.uri, 1);

        char cl[16];
        snprintf(cl, sizeof(cl), "%d", c->parser.content_len);
        setenv("CONTENT_LENGTH", cl, 1);

        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        setenv("SERVER_SOFTWARE", "tiny-httpd/0.4", 1);

        extern char **environ;
        char *argv[] = { (char *)script_path, NULL };
        execve(script_path, argv, environ);
        _exit(127);
    }

    close(pipe_out[1]);

    /* POST：写入 stdin 管道 */
    if (has_stdin) {
        close(pipe_in[0]);
        const char *body = c->rbuf + c->rpos;
        int to_write = c->parser.content_len;
        int written = 0;
        while (written < to_write) {
            ssize_t n = write(pipe_in[1], body + written, to_write - written);
            if (n > 0) written += n;
            else break;
        }
        close(pipe_in[1]);
    }

    /* 异步 CGI：将管道读端注册到 epoll */
    set_nonblock(pipe_out[0]);

    c->cgi_fd  = pipe_out[0];
    c->cgi_pid = pid;
    c->cgi_olen = 0;
    c->state = STATE_CGI;

    /* 先取消注册客户端 fd，再注册管道 fd */
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);

    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_out[0], &ev) == -1) {
        close(pipe_out[0]);
        waitpid(pid, NULL, 0);
        /* 恢复客户端 fd 注册 */
        ev.events = EPOLLIN;
        epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
        send_error(epfd, c, 500, "Internal Server Error");
        return;
    }

    printf("[INFO] %d CGI fork pid=%d: %s\n", c->fd, pid, script_path);
}

/* ===================== 路由 ===================== */

/*
 * HTTP 请求路由分发器
 *
 * 按优先级匹配三条路由：
 *   1. /api/status        → 内置 JSON 端点（服务器运行状态）
 *   2. /cgi-bin/*         → 异步 CGI 执行（需脚本存在且可执行）
 *   3. 其他 URI (仅 GET)  → 静态文件服务（realpath 防护路径穿越）
 *
 * 安全措施：
 *   - CGI 路由先 access(X_OK) 验证，404 vs 403 区分不存在和无权限
 *   - 静态文件路由 realpath() 解析后前缀匹配 g_docroot，防御 ../ 穿越
 *   - POST 到非 CGI 路径返回 405 Method Not Allowed
 */
void route_request(int epfd, conn_t *c)
{
    /* /api/status */
    if (strcmp(c->request.uri, "/api/status") == 0) {
        serve_api_status(epfd, c);
        return;
    }

    char uri[1024];
    if (strcmp(c->request.uri, "/") == 0)
        strcpy(uri, "/index.html");
    else {
        strncpy(uri, c->request.uri, sizeof(uri) - 1);
        uri[sizeof(uri) - 1] = '\0';
    }

    /* CGI 路由 */
    if (strncmp(uri, "/cgi-bin/", 9) == 0) {
        char script_path[2048];
        snprintf(script_path, sizeof(script_path), "%s%s", g_docroot, uri);

        /* 检查文件是否存在且可执行 */
        if (access(script_path, X_OK) != 0) {
            send_error(epfd, c, 404, "Not Found");
            return;
        }
        exec_cgi(epfd, c, script_path);
        return;
    }

    /* 静态文件：只允许 GET */
    if (c->request.method != HTTP_GET) {
        send_error(epfd, c, 405, "Method Not Allowed");
        return;
    }

    char full_path[2048];
    snprintf(full_path, sizeof(full_path), "%s%s", g_docroot, uri);

    char resolved[2048];
    if (realpath(full_path, resolved) == NULL) {
        /* realpath 失败：检查是否是路径穿越尝试 */
        if (strstr(uri, "..") != NULL ||
            strcasestr(uri, "%2e%2e") != NULL ||
            strcasestr(uri, "%2e.") != NULL ||
            strcasestr(uri, ".%2e") != NULL) {
            send_error(epfd, c, 403, "Forbidden");
        } else {
            send_error(epfd, c, 404, "Not Found");
        }
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

    /*
     * 循环调用 accept()，一次处理完所有就绪连接
     * （适用于边缘触发模式，或批量收取以提高效率）
     */
    while (1) {
        /*
         * 每次循环前必须重置 addrlen，因为 accept 会修改它
         * （在某些实现中，传入较小的长度会被截断）
         */
        addrlen = sizeof(client_addr);
        int fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (fd == -1) {
            /* 没有更多就绪连接，正常退出循环 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            /*
             * 进程或系统文件描述符耗尽，属于暂时性错误，
             * 不应彻底终止处理，简单延时后重试（让出 CPU）
             */
            if (errno == EMFILE || errno == ENFILE) {
                usleep(1000);    /* 1ms 后再尝试，避免忙等 */
                continue;
            }
            /* 其他不可恢复错误，打印并停止 accept */
            perror("accept");
            break;
        }

        /*
         * 简单的连接数上限检查：假定 MAX_CONN 为数组容量，
         * 且 g_conns 以 fd 为索引。若新 fd 超出范围，直接拒绝。
         * 更好的做法是根据实际已用连接数或数组空闲槽位判断。
         */
        if (fd >= MAX_CONN) {
            close(fd);
            continue;
        }

        /* 将新套接字设为非阻塞，失败则释放 fd 并继续 */
        if (set_nonblock(fd) == -1) {
            close(fd);
            continue;
        }

        /* 分配连接状态结构体，并初始化零值 */
        conn_t *c = calloc(1, sizeof(conn_t));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd          = fd;
        c->state       = STATE_READING;
        c->last_active = time(NULL);
        http_parser_init(&c->parser);

        /* 将新连接注册到全局连接表 */
        g_conns[fd] = c;

        /* 构造 epoll 事件，关联连接结构体指针 */
        struct epoll_event ev;
        ev.events   = EPOLLIN;     /* 监听可读事件 */
        ev.data.ptr = c;           /* 回调时直接获取连接上下文 */
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl ADD");
            /*
             * 注册失败，必须回滚已分配资源：
             * 1. 从全局表中清除指针
             * 2. 释放连接结构体内存
             * 3. 关闭套接字
             */
            g_conns[fd] = NULL;
            free(c);
            close(fd);
            continue;
        }

        /* 更新全局统计计数（线程安全） */
        pthread_mutex_lock(&g_stats.lock);
        g_stats.conn_count++;
        pthread_mutex_unlock(&g_stats.lock);

        /* 提取客户端 IP 和端口用于日志 */
        char ip_buf[32];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        printf("[INFO] 新连接: fd=%d, %s:%d\n",
               fd, ip_buf, ntohs(client_addr.sin_port));
        log_server(g_logger, LOG_INFO, "fd=%d 新连接来自 %s:%d",
                   fd, ip_buf, ntohs(client_addr.sin_port));
    }
}

/*
 * 处理客户端可读事件 (EPOLLIN)
 * recv 数据追加到 rbuf[rlen]，随后驱动 http_parser 状态机。
 * 解析完成 (parse_request=0) 后调用 route_request() 分发。
 * 读错误或对端关闭 (n==0) 时清理连接。
 */
void handle_read(int epfd, conn_t *c)
{
    c->last_active = time(NULL);

    int n = read(c->fd, c->rbuf + c->rlen, BUF_SIZE - c->rlen);

    if (n == 0) { close_conn(epfd, c); return; }      /* 对端正常关闭 */
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  /* 非阻塞读空 */
        perror("read"); close_conn(epfd, c); return;
    }
    c->rlen += n;

    if (parse_request(c) == 0) {
        pthread_mutex_lock(&g_stats.lock);
        g_stats.req_total++;
        pthread_mutex_unlock(&g_stats.lock);
        route_request(epfd, c);
    }
}

/*
 * 处理客户端可写事件 (EPOLLOUT)
 * 非阻塞 write() 分批发送 wbuf[wpos..wlen]。
 * 全部发送完毕 (wpos>=wlen) 后关闭连接——HTTP/1.0 无 keep-alive。
 */
void handle_write(int epfd, conn_t *c)
{
    c->last_active = time(NULL);

    int n = write(c->fd, c->wbuf + c->wpos, c->wlen - c->wpos);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  /* 发送缓冲区满 */
        perror("write"); close_conn(epfd, c); return;
    }
    c->wpos += n;
    if (c->wpos >= c->wlen) close_conn(epfd, c);   /* 全部发送完毕 */
}

/*
 * 关闭连接 — 完整资源清理
 *
 * 清理顺序：
 *   1. epoll 注销（CGI 状态注销管道 fd，否则注销客户端 fd）
 *   2. close(fd) + close(cgi_fd)  关闭所有文件描述符
 *   3. 若 CGI 子进程仍存活 → kill(SIGKILL) + waitpid 强制终止
 *   4. g_stats.conn_count--       更新全局连接计数
 *   5. g_conns[fd] = NULL         从连接池移除
 *   6. free(c)                    释放堆内存
 *
 * 注意：CGI 状态下 epoll 监听的 fd 是 cgi_fd（管道），而非
 * c->fd（客户端 socket）。关闭时必须分别处理注册注销。
 */
void close_conn(int epfd, conn_t *c)
{
    if (c->state == STATE_CGI) {
        /* CGI 状态：epoll 注册的是管道 fd */
        if (c->cgi_fd > 0) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, c->cgi_fd, NULL);
            close(c->cgi_fd);
            c->cgi_fd = -1;
        }
    } else {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    }
    close(c->fd);

    if (c->cgi_fd > 0) {
        close(c->cgi_fd);
        c->cgi_fd = -1;
    }
    if (c->cgi_pid > 0) {
        kill(c->cgi_pid, SIGKILL);          /* 强制终止 CGI 子进程 */
        waitpid(c->cgi_pid, NULL, WNOHANG);  /* 非阻塞回收，避免僵尸 */
    }

    pthread_mutex_lock(&g_stats.lock);
    g_stats.conn_count--;
    if (g_stats.conn_count < 0) g_stats.conn_count = 0;
    pthread_mutex_unlock(&g_stats.lock);

    g_conns[c->fd] = NULL;
    printf("[INFO] fd=%d 连接关闭\n", c->fd);
    free(c);
}

/* ===================== 超时与僵尸进程 ===================== */

/*
 * 超时检查：遍历 g_conns 数组，关闭超过 TIMEOUT(30s) 无活动的连接
 * 由主循环每约 10 秒触发一次（epoll_wait 1s 超时 × 10 次）
 * 防止慢客户端、半开连接无限占用槽位
 */
void check_timeout(int epfd)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CONN; i++) {
        conn_t *c = g_conns[i];
        if (!c) continue;
        if (now - c->last_active > TIMEOUT) {
            printf("[WARN] fd=%d 超时 (%lds)，关闭\n", c->fd, (long)(now - c->last_active));
            log_server(g_logger, LOG_WARN, "fd=%d 超时，关闭", c->fd);
            close_conn(epfd, c);
        }
    }
}

/*
 * 回收 CGI 僵尸子进程
 * waitpid(-1, ..., WNOHANG) 以非阻塞方式回收所有已终止子进程。
 * 与 check_timeout() 同频调用（约每 10 秒）。
 *
 * 注意：WSL1 下 /proc 文件系统不完整，waitpid(-1,...) 可能无法
 * 检测某些 zombie，导致少量资源泄漏。真实 Linux 内核下完全正确。
 */
void reap_zombies(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}
