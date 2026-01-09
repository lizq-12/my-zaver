#!/bin/bash

set -euo pipefail

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "=== Starting Functional Test ==="

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT_DIR"

LOG_FILE="${ROOT_DIR}/tests/functional_test.server.log"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        # Kill the whole process group (master + workers)
        kill -TERM -- "-${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

# 1. 智能查找可执行文件位置
# 既然我们在根目录运行，通常 build 就在 ./build
if [ -f "./build/zaver" ]; then
    BIN_PATH="./build/zaver"
elif [ -f "./build/src/zaver" ]; then
    BIN_PATH="./build/src/zaver"
else
    echo -e "${RED}Error: Could not find 'zaver' executable!${NC}"
    echo "Searching in ./build directory:"
    find ./build -name "zaver"
    exit 1
fi

echo "Found server binary at: $BIN_PATH"

# 2. 启动服务器
rm -f "$LOG_FILE"
setsid "$BIN_PATH" -c "$ROOT_DIR/zaver.conf" >"$LOG_FILE" 2>&1 &
SERVER_PID=$!
echo "Server started with PID $SERVER_PID"

# 3. 等待服务器就绪（避免固定 sleep 的偶发失败）
READY=0
for i in $(seq 1 50); do
    CODE=$(curl --max-time 1 -o /dev/null -s -w "%{http_code}" http://127.0.0.1:3000/index.html || true)
    if [[ "$CODE" != "000" ]]; then
        READY=1
        break
    fi
    sleep 0.1
done

if [[ "$READY" -ne 1 ]]; then
    echo -e "${RED}Server did not become ready in time.${NC}"
    echo "--- server log (tail) ---"
    tail -n 200 "$LOG_FILE" || true
    exit 1
fi

# 4. 发送测试请求
echo "Sending request to http://127.0.0.1:3000/index.html ..."
# 加上 -v 可以看到详细连接过程，方便调试
HTTP_CODE=$(curl -o /dev/null -s -w "%{http_code}" http://127.0.0.1:3000/index.html)

# 5. 验证结果
if [ "$HTTP_CODE" -eq 200 ]; then
    echo -e "${GREEN}Test Passed! Server returned HTTP 200.${NC}"
    RESULT=0
else
    echo -e "${RED}Test Failed! Server returned HTTP $HTTP_CODE${NC}"
    echo "--- server log (tail) ---"
    tail -n 200 "$LOG_FILE" || true
    RESULT=1
fi

exit $RESULT
