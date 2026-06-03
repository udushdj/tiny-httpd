# tiny-httpd

轻量级 HTTP/1.1 服务器与 CGI 引擎，基于 epoll 的单线程事件驱动架构。

## 快速开始

```bash
make          # 编译
make run      # 启动服务器（默认端口 8080）
```

浏览器打开 `http://127.0.0.1:8080/index.html`

## 功能

- epoll 事件驱动，单线程支持 1024+ 并发
- HTTP/1.1 GET / POST 请求解析
- 静态文件服务（MIME 映射、路径穿越防护）
- CGI 动态请求（fork + exec，管道通信，环境变量传递）
- 连接超时管理与僵尸进程回收
- 非阻塞 IO，正确处理 EAGAIN

## 技术栈

C 语言，Linux 系统调用（epoll、fork、exec、pipe、mmap），HTTP/1.1 协议。

- **编译**：GCC >= 4.8，GNU Make
- **运行**：Linux (x86_64 / ARM)

## 目录结构

```
轻量化http服务器CGI引擎/
├── Makefile
├── README.md
├── 项目书.md
├── demo.sh                     # 一键演示（Day 5 完成）
├── src/
│   └── main.c                  # 主程序（Day 1 开始编写）
├── www/
│   ├── index.html
│   ├── style.css
│   ├── 404.html
│   ├── 500.html
│   └── cgi-bin/
│       ├── hello.py            # GET 参数回显
│       ├── echo.py             # POST body 回显
│       └── status              # 系统状态（Shell）
└── test/
    └── test.sh                 # 自动化测试脚本
```

> 当前已建立完整目录结构和静态资源。`src/main.c` 将在 Day 1 编码阶段创建。

## 使用示例

```bash
# 指定端口
./tiny-httpd 9090

# 静态文件
curl -v http://127.0.0.1:8080/index.html

# CGI GET 请求
curl "http://127.0.0.1:8080/cgi-bin/hello.py?name=world"

# CGI POST 请求
curl -X POST -d "key=value" http://127.0.0.1:8080/cgi-bin/echo.py

# 压力测试
ab -n 1000 -c 100 http://127.0.0.1:8080/index.html

# 内存检查
valgrind --leak-check=full ./tiny-httpd 8080
```

## 架构

```
客户端请求 → 监听 socket → epoll 事件循环
  ├─ 新连接 → accept → 注册 EPOLLIN
  ├─ EPOLLIN → 读取数据 → HTTP 解析状态机
  │     ├─ 静态文件 → 安全检查 → mmap → EPOLLOUT
  │     └─ CGI 请求 → pipe → fork → 子进程 exec
  │                   父进程监听管道 → 解析输出 → EPOLLOUT
  ├─ EPOLLOUT → 非阻塞 write → 完成后关闭
  └─ 超时检查 → 清理空闲连接与僵尸进程
```

## 安全特性

- `realpath` 解析后必须位于 `DOCROOT` 下，防止路径穿越
- CGI 脚本被 `kill` 后自动回收，无僵尸进程残留
- 连接 30 秒无活动自动断开

## License

MIT
