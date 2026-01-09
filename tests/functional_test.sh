#!/bin/bash

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "=== Starting Functional Test ==="

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
$BIN_PATH &
SERVER_PID=$!
echo "Server started with PID $SERVER_PID"

# 3. 等待几秒让服务器初始化
sleep 3

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
    # 如果失败，打印一下最后几行日志看看（如果有日志文件的话）
    RESULT=1
fi

# 6. 清理战场
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null

exit $RESULT
