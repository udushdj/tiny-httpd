# tiny-httpd

轻量级 HTTP/1.1 服务器与 CGI 引擎，基于 epoll 事件驱动架构。
支持静态文件服务、CGI 动态请求、日志系统、连接超时管理。

## 运行拓扑

```
客户端 (curl/浏览器)  ──HTTP请求──→  tiny-httpd (服务器)
                        ←──HTTP响应──    -p 8080
```

## 快速开始

```bash
# WSL 本地编译运行（x86_64）
./auto_build.sh run

# 功能测试
./auto_build.sh test

# 压力测试
./auto_build.sh stress

# ARM 交叉编译
./auto_build.sh arm

# 部署到开发板
./auto_build.sh deploy
```

## 功能

- **epoll 事件驱动**：单线程非阻塞 IO，高效处理并发连接
- HTTP/1.0 GET / POST 请求解析
- 静态文件服务（MIME 映射、路径穿越防护）
- CGI 动态请求（fork + execve + pipe 异步通信）
- `/api/status` JSON 接口（连接数、请求数、运行时间）
- 连接超时管理（30s）与僵尸进程回收
- 日志文件系统：access.log / server.log / error.log，支持自动轮转
- 命令行参数解析（-p port -d docroot -l logdir）
- SIGPIPE 信号处理
- 多线程压力测试工具（stress + webbench）

## 技术栈

| 层面 | 技术 |
|------|------|
| 语言 | C (C11) |
| 网络 | epoll, non-blocking IO, TCP |
| 进程 | fork, execve, pipe, waitpid, dup2 |
| 构建 | CMake 3.10+ |
| 编译 | GCC / arm-linux-gcc 5.4.0 |
| 平台 | x86_64 Linux / ARM 32-bit Linux |

## 命令行参数

```
./tiny-httpd -p 8080 -d ./www -l logs
  -p PORT     监听端口 (默认 8080)
  -d DIR      DOCROOT 目录 (默认 ./www)
  -l DIR      日志目录 (默认 logs)
  -h          显示帮助
```

## 目录结构

```
tiny-httpd/
├── CMakeLists.txt              # CMake 构建配置
├── cmake/
│   └── arm-toolchain.cmake     # ARM 交叉编译工具链
├── auto_build.sh               # 一键构建脚本
├── README.md
├── 项目书.md
├── src/
│   ├── main.c                  # 入口：参数解析 + epoll 事件循环
│   ├── server.c                # HTTP 引擎 + CGI 异步处理
│   ├── server.h                # 数据结构 + 接口声明
│   ├── logger.c                # 日志文件系统实现
│   ├── logger.h                # 日志接口定义
│   ├── gui.c                   # 保留（LVGL 面板已取消）
│   ├── http_parser.c           # HTTP 请求解析状态机
│   └── http_parser.h
├── www/
│   ├── index.html
│   ├── style.css
│   └── cgi-bin/
│       ├── test.c              # CGI 测试程序（C 编译）
│       ├── hello.py            # GET 参数回显
│       ├── echo.py             # POST body 回显
│       └── status              # Shell: 系统状态
├── test/
│   ├── test.sh                 # 自动化功能测试
│   ├── stress.c                # 多线程 HTTP 压力测试
│   └── webbench.c              # WebBench 风格持续压测
└── .trae/                      # 调试文档
    ├── debug_buffer_overflow.md
    ├── debug_cgi_fix.md
    └── stress_test.md
```

## 架构

```
                    ┌─────────────────┐
                    │   epoll_wait    │
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
        ┌──────────┐  ┌──────────┐  ┌──────────┐
        │accept新  │  │EPOLLIN   │  │EPOLLOUT  │
        │连接      │  │handle_read│  │handle_write
        └──────────┘  └─────┬────┘  └──────────┘
                            │
                     ┌──────▼──────┐
                     │route_request│
                     └──┬─────┬───┘
                        │     │
                   静态文件  CGI
                        │     │
                   ┌────▼──┐ ┌▼────────────┐
                   │sendfile│ │fork+execve  │
                   │响应    │ │异步读管道    │
                   └────────┘ └─────────────┘
```

## 构建命令

| 命令 | 效果 |
|------|------|
| `./auto_build.sh` | 编译 Debug 并运行 |
| `./auto_build.sh release` | 编译 Release 版本 |
| `./auto_build.sh debug` | 编译 Debug 版本（不运行） |
| `./auto_build.sh test` | 编译并运行功能测试 |
| `./auto_build.sh stress` | 编译并运行压力测试 |
| `./auto_build.sh arm` | ARM 交叉编译 |
| `./auto_build.sh deploy` | ARM 编译 + SCP 部署 |
| `./auto_build.sh clean` | 清理构建产物 |

### 手动 CMake 构建

```bash
# Debug 构建
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --parallel

# Release 构建
mkdir -p build-release && cd build-release
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel

# ARM 交叉编译
mkdir -p build-arm && cd build-arm
cmake .. -DCMAKE_TOOLCHAIN_FILE=cmake/arm-toolchain.cmake
cmake --build . --parallel
```

## 压力测试

### stress（短时快速测试）

```bash
# 编译
gcc -Wall -O2 -pthread -o test/stress-test test/stress.c

# 运行
test/stress-test -h 127.0.0.1 -p 8080 -n 1000 -c 10
```

### webbench（持续压测）

```bash
# 编译
gcc -Wall -O2 -pthread -o test/webbench test/webbench.c

# 运行：2000 并发，30 秒
test/webbench -h 127.0.0.1 -p 8080 -c 2000 -t 30
```

### 测试结果

| 指标 | 值 |
|------|------|
| 并发客户端 | 2000 |
| 持续时间 | 30s |
| 总请求 | 45,082 |
| 成功率 | 100% |
| QPS | 1502.7 |
| 带宽 | 304.1 KB/s |

## 安全特性

- `realpath` 路径穿越防护（支持 URL 编码的 `..`）
- CGI 子进程 kill + waitpid 回收，零僵尸进程
- 连接 30 秒无活动自动断开
- SIGPIPE 信号忽略（防止 write 到已关闭连接崩溃）
- 日志系统 pthread_mutex 线程安全

## WSL 同步命令

每次修改后，在 WSL 环境下运行以下命令同步代码：

```bash
rsync -av --delete /mnt/c/Users/46499/Desktop/轻量化http服务器CGI引擎/ ~/codepath/轻量化http服务器CGI引擎/
```

## 调试文档

所有调试记录存储在 `.trae/` 目录：

- [`.trae/debug_buffer_overflow.md`](.trae/debug_buffer_overflow.md) - BSS 段过大导致的 buffer overflow 误报修复
- [`.trae/debug_cgi_fix.md`](.trae/debug_cgi_fix.md) - CGI Empty Reply 问题修复（描述符泄漏）
- [`.trae/stress_test.md`](.trae/stress_test.md) - 压力测试完整报告

## License

MIT
