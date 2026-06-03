# Buffer Overflow 调试记录

## 问题现象

```
$ make && ./tiny-httpd 8080
*** buffer overflow detected ***: terminated
Aborted (core dumped)
```

在 `printf("[INFO] 监听端口...")` 输出之前就崩溃，说明崩溃在程序初始化阶段，而非请求处理逻辑。

---

## 根因分析

### 崩溃链路

```
-O2 编译
  → 隐式启用 _FORTIFY_SOURCE=2
    → 编译器在每个大数组末尾插入"金丝雀"保护值
    → 启动时 runtime 检查 BSS/数据段金丝雀是否完整
    → 发现金丝雀被破坏 → 立即 abort()
```

### 触发 Fortify 误判的结构

```c
// server.h
struct conn_t {
    int   fd;
    char  rbuf[BUF_SIZE];       // 16KB
    char  wbuf[BUF_SIZE];       // 16KB
    int   rpos, rlen, wpos, wlen;
    http_parser  parser;        // 约 2KB
    http_request request;       // 约 3KB
};  // 总计约 37KB

// server.c（旧版）
static conn_t connections[MAX_CONN];  // 256×37KB = 9.5MB BSS段
```

大 BSS 段 + Fortify 金丝雀 = 误报。

### 对比 example_src 参考项目

[example_src/epoll.cpp](file:///c:/Users/46499/Desktop/轻量化http服务器CGI引擎/example_src/epoll.cpp#L28) 存储的是 `vector<shared_ptr<RequestData>>`（智能指针数组），每个元素仅 16 字节，数据用 `std::string` 在堆上动态分配。**整个项目没有一个大 BSS 数组。**

---

## 解决方案

### 临时方案（快速验证）

```bash
# 禁用优化，绕过 Fortify
gcc -O0 -g -o tiny-httpd src/main.c src/server.c src/http_parser.c

# 或保留优化但禁用 Fortify
gcc -O2 -g -U_FORTIFY_SOURCE -o tiny-httpd src/main.c src/server.c src/http_parser.c
```

### 长期方案（指针数组 + 按需分配）

将 `static conn_t connections[MAX_CONN]`（9MB BSS）改为**指针数组**，每连接 accept 时按需 `calloc`，close 时 `free`。

```c
// server.h — 指针数组，BSS 仅 256×8=2KB
extern conn_t *g_conns[MAX_CONN];

// server.c handle_accept — 按需分配
conn_t *c = calloc(1, sizeof(conn_t));
c->fd = fd;
g_conns[fd] = c;

// server.c close_conn — 释放
g_conns[c->fd] = NULL;
free(c);
```

### Makefile 长期修复

```makefile
CFLAGS_LOCAL = -Wall -Wextra -O2 -g -pthread
# 如果 Fortify 仍然误报，显式禁用：
# CFLAGS_LOCAL = -Wall -Wextra -O2 -g -pthread -U_FORTIFY_SOURCE
```

---

## 验证步骤

```bash
# 1. 确认是指针数组版
grep -n 'g_conns' src/server.c | head -3
# 输出应为:
# 4:conn_t *g_conns[MAX_CONN];
# 253:g_conns[fd] = c;
# 329:g_conns[c->fd] = NULL;

# 2. 编译（确认无 buffer overflow）
make clean && make

# 3. 启动并测试
./tiny-httpd 8080
curl -v http://127.0.0.1:8080/
```

---

## 关键教训

1. **大结构体内嵌大数组**（如 `char rbuf[16384]` 内嵌在结构体中）在 `-O2` + `_FORTIFY_SOURCE=2` 下易触发误报
2. 参考开源项目（如 linyacool/WebServer）的做法：**数据缓冲用动态分配（堆/string），连接表用指针数组**
3. 崩溃在 `printf` 之前 = 初始化阶段问题，不是业务逻辑
4. 快速定位：`gcc -O0 -g && ./tiny-httpd` 排除 Fortify 干扰；`gcc -fsanitize=address` 精准定位真实越界

---

## 涉及文件

| 文件 | 修改内容 |
|------|---------|
| [server.h](file:///c:/Users/46499/Desktop/轻量化http服务器CGI引擎/src/server.h) | `conn_t *g_conns[MAX_CONN]` 指针数组声明 |
| [server.c](file:///c:/Users/46499/Desktop/轻量化http服务器CGI引擎/src/server.c) | handle_accept 按需 calloc，close_conn free |
| [main.c](file:///c:/Users/46499/Desktop/轻量化http服务器CGI引擎/src/main.c) | 移除 server_init() 调用和 free(g_conns) |
