# tiny-httpd

多线程 HTTP/1.1 服务器与 CGI 引擎，基于 epoll + SO_REUSEPORT + 线程池架构。
部署在 ARM aarch64 开发板，带 LVGL 服务端状态面板，Ubuntu 风格 UI。

## 运行拓扑

```
你的电脑 (客户端)  ──HTTP请求──→  ARM开发板 (服务器)
  curl / 浏览器    ←──HTTP响应──    tiny-httpd -p 8080 -t 4
                                    + LVGL 面板 (屏幕)
```

## 快速开始

```bash
# WSL 本地编译验证（x86_64）
make && ./tiny-httpd -p 8080 -t 4

# ARM 交叉编译 + 部署
make arm                 # 交叉编译
make deploy              # SCP 上传到开发板

# 开发板上运行（4个worker线程）
./tiny-httpd -p 8080 -t 4     # 屏幕显示 LVGL 面板
```

## 功能

- **多线程线程池 + epoll**：SO_REUSEPORT 内核级负载均衡，每 worker 独立事件循环
- HTTP/1.1 GET / POST 请求解析
- 静态文件服务（MIME 映射、路径穿越防护）
- CGI 动态请求（fork + exec、管道通信、环境变量传递）
- `/api/status` JSON 接口（连接数、请求数、运行时间、worker数）
- 连接超时管理（30s）与僵尸进程回收
- **日志文件系统**：access.log / server.log / error.log，支持自动轮转
- **编译宏开关**：DEBUG / TEST_MODE / ENABLE_CGI / ENABLE_LVGL 灵活切换
- **LVGL 服务端状态面板**：实时显示各 worker 连接数、请求分布、CPU/内存
- **多线程压力测试工具**：`stress_test` 可配置并发数/请求数

## 技术栈

| 层面 | 技术 |
|------|------|
| 语言 | C (GNU99) |
| 网络 | epoll, SO_REUSEPORT, non-blocking IO, TCP |
| 并发 | pthread 线程池（N worker + 1 LVGL） |
| 进程 | fork, execve, pipe, waitpid |
| GUI | LVGL v8 |
| 日志 | 文件持久化 + 内存环形缓冲，独立 mutex |
| 编译 | GCC / arm-linux-gcc 5.4.0 |
| 平台 | ARM 32-bit Linux / x86_64 Linux |

## 命令行参数

```
./tiny-httpd -p 8080 -t 4 -d ./www -l logs
  -p PORT     监听端口 (默认 8080)
  -t N        worker 线程数 (默认 = CPU核心数)
  -d DIR      DOCROOT 目录 (默认 ./www)
  -l DIR      日志目录 (默认 logs)
```

## 目录结构

```
轻量化http服务器CGI引擎/
├── Makefile
├── CMakeLists.txt              # CMake交叉编译（含LVGL）
├── README.md
├── 项目书.md
├── src/
│   ├── main.c                  # 入口：解析参数 + 创建worker + LVGL
│   ├── server.c                # HTTP引擎（每worker独立epoll）
│   ├── server.h                # 共享状态 + worker参数定义
│   ├── logger.c                # 日志文件系统
│   ├── logger.h                # 日志接口 + 编译宏定义
│   ├── gui.c                   # LVGL界面（Ubuntu Yaru风格）
│   ├── gui.h
│   ├── http_parser.c           # HTTP解析器
│   └── http_parser.h
├── www/
│   ├── index.html
│   ├── style.css
│   ├── 404.html / 500.html
│   └── cgi-bin/
│       ├── hello.py            # GET 参数回显
│       ├── echo.py             # POST body 回显
│       └── status              # Shell: 系统状态
├── test/
│   ├── test.sh                 # 自动化测试脚本
│   └── stress.c                # 多线程压力测试工具
└── logs/                       # 运行时自动创建
    ├── access.log              # 请求日志（时间 IP 方法 URI 状态码 耗时 worker_id）
    ├── server.log              # 服务器事件日志
    └── error.log               # 错误日志
```

## 架构

```
                  Linux 内核 (SO_REUSEPORT)
             ┌───────┬───────┬───────┬───────┐
        listen_fd listen_fd listen_fd listen_fd
             │       │       │       │       │
        ┌────▼──┐ ┌─▼──────┐┌─▼──────┐┌─▼──────┐
        │Worker0│ │Worker1 ││Worker2 ││Worker3 │
        │独立epoll│独立epoll│独立epoll│独立epoll│
        └───┬───┘ └───┬────┘└───┬────┘└───┬────┘
            │         │         │         │
            └─────────┼─────────┼─────────┘
                      │         │
              ┌───────▼──┐ ┌───▼──────────┐
              │ 共享状态  │ │ logger(日志)  │
              │ + mutex  │ │ access/server │
              └───────┬──┘ │ /error.log    │
                      │    └───────────────┘
              ┌───────▼──────────┐
              │  LVGL UI 主线程   │
              │  lv_timer(200ms) │
              └──────────────────┘
```

## 压力测试

```bash
# 编译压力测试工具
make stress

# 启动服务器（4 worker）
./tiny-httpd -p 8080 -t 4 &

# 运行压力测试（10000请求，50并发线程）
./stress_test -h 127.0.0.1 -p 8080 -n 10000 -c 50
# [STRESS] 总请求: 10000, 并发: 50 线程
# [STRESS] 耗时: 2.34s, QPS: 4273.5
# [STRESS] 成功: 10000, 失败: 0
# [STRESS] 最小: 2ms, 平均: 11ms, 最大: 89ms

# 高并发压力测试
./stress_test -h 127.0.0.1 -p 8080 -n 50000 -c 200
```

## 编译模式

| 命令 | 效果 |
|------|------|
| `make` | x86_64 标准编译（LOG_INFO, 日志写文件） |
| `make debug` | 调试模式（-DDEBUG, 控制台输出, -O0） |
| `make test` | 测试模式（-DTEST_MODE, 100请求自动退出） |
| `make arm` | ARM 32-bit 交叉编译（无 LVGL） |
| `make arm-full` | ARM 交叉编译（含 LVGL 源码，需设置 LVGL_PATH） |
| `make headless` | ARM 无头模式（无 GUI） |
| `make stress` | 编译压力测试工具 |
| `make bench` | 一键启动服务器 + 运行压力测试 |

## 安全特性

- `realpath` 路径穿越防护（403）
- CGI 子进程 kill + waitpid 回收，零僵尸进程
- 连接 30 秒无活动自动断开
- 共享状态 + 日志各自独立 pthread_mutex 保护

## WSL 同步命令

每次修改后，在 WSL 环境下运行以下命令同步代码：

```bash
# 将 Windows 代码同步到 WSL
cp -r /mnt/c/Users/46499/Desktop/轻量化http服务器CGI引擎/* ~/codepath/轻量化http服务器CGI引擎/

# 或使用 rsync（推荐，仅同步变更文件）
rsync -av --delete /mnt/c/Users/46499/Desktop/轻量化http服务器CGI引擎/ ~/codepath/轻量化http服务器CGI引擎/
```

## 调试文档

所有调试记录存储在 `.trae/` 目录：

- [`.trae/debug_buffer_overflow.md`](.trae/debug_buffer_overflow.md) - BSS 段过大导致的 buffer overflow 误报修复
- [`.trae/debug_cgi_fix.md`](.trae/debug_cgi_fix.md) - CGI Empty Reply 问题修复（描述符泄漏）

## License

MIT
