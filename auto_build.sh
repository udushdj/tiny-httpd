#!/bin/bash
# auto_build.sh - tiny-httpd 快速编译脚本
# 用法:
#   ./auto_build.sh              x86_64 本地编译 + 运行
#   ./auto_build.sh arm          交叉编译 ARM 版本
#   ./auto_build.sh deploy       交叉编译 + SCP 部署到开发板
#   ./auto_build.sh clean        清理编译产物
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="tiny-httpd"
SRCS="src/main.c src/server.c src/http_parser.c"

# ARM 交叉编译器
CROSS_PREFIX="/home/wz/usr/local/arm/5.4.0/usr/bin/arm-linux-"
ARM_CC="${CROSS_PREFIX}gcc"
BOARD_IP="${BOARD_IP:-192.168.1.100}"
BOARD_DIR="${BOARD_DIR:-/root}"

cd "$PROJECT_DIR"

build_local() {
    echo "=== 编译 x86_64 本地版本 ==="
    gcc -Wall -Wextra -O2 -g -pthread -U_FORTIFY_SOURCE -o "$TARGET" $SRCS -lpthread
    echo "编译完成: $(file "$TARGET" | cut -d: -f2-)"
}

build_cgi() {
    echo "=== 编译 CGI 测试程序 ==="
    if [ -f "www/cgi-bin/test.c" ]; then
        gcc -Wall -O2 -o www/cgi-bin/test www/cgi-bin/test.c
        chmod +x www/cgi-bin/test
    fi
    chmod +x www/cgi-bin/* 2>/dev/null || true
}

build_arm() {
    echo "=== 交叉编译 ARM 版本 ==="
    if [ ! -x "$ARM_CC" ]; then
        echo "错误: 找不到交叉编译器 $ARM_CC"
        exit 1
    fi
    ARM_SRCS="$SRCS src/logger.c src/gui.c"
    "$ARM_CC" -Wall -Wextra -O2 -g -pthread \
        -I/home/wz/usr/local/openssl-arm/include \
        -o "$TARGET" $ARM_SRCS \
        -lpthread -lm -lrt -ldl
    echo "编译完成: $(file "$TARGET" | cut -d: -f2-)"
}

deploy() {
    build_arm
    echo "=== 部署到开发板 $BOARD_IP ==="
    scp "$TARGET" "root@${BOARD_IP}:${BOARD_DIR}/"
    scp -r www "root@${BOARD_IP}:${BOARD_DIR}/"
    echo "部署完成"
}

clean() {
    rm -f "$TARGET" src/*.o stress_test
    rm -f www/cgi-bin/test
    echo "清理完成"
}

run() {
    build_local
    build_cgi
    echo "=== 启动服务器 ==="
    ./"$TARGET" 8080
}

case "${1:-run}" in
    arm)    build_arm ;;
    deploy) deploy ;;
    clean)  clean ;;
    run)    run ;;
    *)      echo "用法: $0 {run|arm|deploy|clean}" ;;
esac
