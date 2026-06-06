#!/bin/bash
# auto_build.sh - tiny-httpd 构建脚本（基于 CMake）
# 用法:
#   ./auto_build.sh              编译并运行（本地 Debug）
#   ./auto_build.sh release      编译 Release 版本
#   ./auto_build.sh arm          ARM 交叉编译
#   ./auto_build.sh deploy       编译 ARM + SCP 部署
#   ./auto_build.sh clean        清理构建产物
#   ./auto_build.sh test         运行功能测试
#   ./auto_build.sh stress       运行压力测试
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
TARGET="tiny-httpd"
BOARD_IP="${BOARD_IP:-192.168.1.100}"
BOARD_DIR="${BOARD_DIR:-/root}"

cd "$PROJECT_DIR"

# ──── 编译 ────
build() {
    local build_type="${1:-Debug}"
    echo "=== 编译 $build_type 版本 ==="

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE="$build_type"
    cmake --build . --parallel

    echo "编译完成: $(file "$PROJECT_DIR/build/$TARGET" | cut -d: -f2-)"
    cd "$PROJECT_DIR"
}

build_arm() {
    echo "=== 交叉编译 ARM 版本 ==="

    local arm_build_dir="$BUILD_DIR-arm"
    mkdir -p "$arm_build_dir"
    cd "$arm_build_dir"
    cmake .. -DCMAKE_TOOLCHAIN_FILE=cmake/arm-toolchain.cmake
    cmake --build . --parallel

    echo "编译完成: $(file "$arm_build_dir/$TARGET" | cut -d: -f2-)"
    cd "$PROJECT_DIR"
}

build_cgi() {
    echo "=== 编译 CGI 测试程序 ==="
    if [ -f "www/cgi-bin/test.c" ]; then
        gcc -Wall -O2 -o www/cgi-bin/test www/cgi-bin/test.c
        chmod +x www/cgi-bin/test
    fi
    chmod +x www/cgi-bin/* 2>/dev/null || true
}

# ──── 部署 ────
deploy() {
    build_arm
    echo "=== 部署到开发板 $BOARD_IP ==="
    scp "$BUILD_DIR-arm/$TARGET" "root@${BOARD_IP}:${BOARD_DIR}/"
    scp -r www "root@${BOARD_IP}:${BOARD_DIR}/"
    echo "部署完成"
}

# ──── 清理 ────
clean() {
    rm -rf build build-arm
    rm -f src/*.o
    rm -f www/cgi-bin/test
    rm -f test/stress-test test/webbench
    echo "清理完成"
}

# ──── 运行 ────
run() {
    build Debug
    build_cgi
    echo "=== 启动服务器 ==="
    "$BUILD_DIR/$TARGET" -p 8080
}

# ──── 测试 ────
run_tests() {
    build Debug
    build_cgi
    echo "=== 运行功能测试 ==="
    "$BUILD_DIR/$TARGET" -p 8080 &
    local pid=$!
    sleep 1
    bash test/test.sh
    local result=$?
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    return $result
}

# ──── 压力测试 ────
run_stress() {
    build Debug
    "$BUILD_DIR/$TARGET" -p 8080 &
    local pid=$!
    sleep 1

    echo "=== 编译压力测试工具 ==="
    gcc -Wall -O2 -pthread -o test/stress-test test/stress.c
    gcc -Wall -O2 -pthread -o test/webbench test/webbench.c

    echo "=== 短时压力测试 ==="
    test/stress-test -h 127.0.0.1 -p 8080 -n 1000 -c 10

    echo "=== WebBench 持续压测（10秒） ==="
    test/webbench -h 127.0.0.1 -p 8080 -c 100 -t 10

    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
}

# ──── 主逻辑 ────
case "${1:-run}" in
    release) build Release ;;
    debug)   build Debug ;;
    arm)     build_arm ;;
    deploy)  deploy ;;
    clean)   clean ;;
    run)     run ;;
    test)    run_tests ;;
    stress)  run_stress ;;
    *)       echo "用法: $0 {run|release|debug|arm|deploy|clean|test|stress}" ;;
esac
