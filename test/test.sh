#!/bin/bash
# tiny-httpd 自动化测试脚本
# 用法: ./test/test.sh [port]

PORT=${1:-8080}
BASE="http://127.0.0.1:$PORT"
PASS=0
FAIL=0

check() {
    local desc="$1" expected="$2"
    shift 2
    local output=$(curl -s -o /dev/null -w "%{http_code}" "$@" 2>/dev/null)
    if [ "$output" = "$expected" ]; then
        echo "  PASS: $desc ($output)"
        ((PASS++))
    else
        echo "  FAIL: $desc (got $output, expected $expected)"
        ((FAIL++))
    fi
}

echo "=== tiny-httpd 测试 ==="
echo "目标: $BASE"
echo ""

echo "[静态文件]"
check "GET index.html" 200 "$BASE/index.html"
check "GET style.css"  200 "$BASE/style.css"
check "GET 不存在文件"  404 "$BASE/nonexistent"

echo "[路径安全]"
# 使用 %2e%2e URL 编码的 .. 避免 curl 自动规范化路径
check "路径穿越拒绝" 403 "$BASE/%2e%2e/%2e%2e/%2e%2e/etc/passwd"

echo "[CGI GET]"
check "CGI hello.py" 200 "$BASE/cgi-bin/hello.py?name=test"

echo "[CGI POST]"
check "CGI echo.py POST" 200 -X POST -d "data=hello" "$BASE/cgi-bin/echo.py"

echo "[静态 POST 拒绝]"
check "静态文件POST" 405 -X POST "$BASE/index.html"

echo ""
echo "=== 结果: $PASS 通过, $FAIL 失败 ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
