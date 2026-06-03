# CGI Empty Reply 问题修复记录

## 问题现象

```bash
$ curl -v http://127.0.0.1:8080/cgi-bin/test
* Connected to 127.0.0.1 (127.0.0.1) port 8080 (#0)
[INFO] 新连接: fd=5, 127.0.0.1:13066
> GET /cgi-bin/test HTTP/1.1
> Host: 127.0.0.1:8080
* Empty reply from server
* Closing connection 0
curl: (52) Empty reply from server
```

**关键日志**：
- 服务器成功 fork 子进程：`[INFO] 5 CGI fork pid=26143`
- 但 curl 立刻收到空响应

## 根因分析

### 1. CGI 子进程描述符泄漏

**问题代码**：
```c
dup2(pipe_out[1], STDOUT_FILENO);
close(pipe_out[0]); close(pipe_out[1]);
```

**问题**：`dup2` 后直接 `close(pipe_out[1])` 会关闭 **原始管道写端**，但 `dup2` 创建的 STDOUT_FILENO (fd 1) 是新副本。

如果子进程持有的其他描述符（如 stdin=0, stderr=2）未关闭，可能导致管道的 write 引用计数不归零。

### 2. 工作目录不匹配

**问题**：子进程未切换工作目录到 DOCROOT，可能导致 CGI 二进制文件的相对路径解析失败。

### 3. 缺乏 execve 失败处理

**问题代码**：
```c
execve(script_path, argv, environ);
printf("Content-Type: text/plain\r\n\r\nCGI exec failed\r\n");
fflush(stdout);
_exit(1);
```

**问题**：`execve` 失败后，`stdout` 已被重定向到管道，但此时管道的写端引用计数可能不归零（因为 stderr 仍开着）。`_exit(1)` 退出码 1 不够明确。

## 修复方案

### 修复 1：清理子进程描述符

```c
if (pid == 0) {
    /* 子进程：关闭不需要的描述符 */
    close(pipe_out[0]);

    /* 关闭 stdin/stderr，避免管道写端引用计数不归零 */
    close(STDIN_FILENO);
    close(STDERR_FILENO);

    if (dup2(pipe_out[1], STDOUT_FILENO) == -1) {
        _exit(127);
    }
    close(pipe_out[1]);  /* 关闭原始写端，只保留 STDOUT_FILENO 副本 */

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
```

### 修复 2：父进程加强错误处理

```c
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
```

### 修复要点

| 修复项 | 说明 |
|--------|------|
| `close(STDIN_FILENO)` | 避免 stdin 保持管道写端引用计数 |
| `close(STDERR_FILENO)` | 避免 stderr 保持管道写端引用计数 |
| `dup2` 错误检查 | 确保重定向成功 |
| `chdir(g_docroot)` | 确保子进程工作目录正确 |
| `_exit(127)` | 使用标准 shell 退出码表示命令未找到 |
| `cgi_output[total] = '\0'` | 确保字符串 null-terminate |
| `total == 0` 检查 | 无输出时返回 500 而非空响应 |

## 验证命令

```bash
cd /home/wz/codepath/轻量化http服务器CGI引擎
pkill -f tiny-httpd
./auto_build.sh run

# 另一个终端
curl -v http://127.0.0.1:8080/cgi-bin/test
```

## 参考

- POSIX execve: 失败时返回 -1，不修改 errno
- CGI 规范: 子进程 stdout 输出 HTTP 头 + 空行 + body
- _exit(127): shell 标准退出码，表示命令未找到或无法执行
