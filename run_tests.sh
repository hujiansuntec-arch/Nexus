#!/bin/bash
# Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
# 测试脚本 - 编译并运行所有测试

set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  LibRPC 测试套件                      ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

# 清理旧的共享内存
echo -e "${YELLOW}[1/4] 清理共享内存...${NC}"
rm -f /dev/shm/librpc_shm_v2
echo -e "${GREEN}✓ 清理完成${NC}\n"

# 编译测试程序
echo -e "${YELLOW}[2/4] 编译测试程序...${NC}"

COMMON_FLAGS="-std=c++14 -O2 -I./include -pthread -lrt"
SOURCES="src/NodeImpl.cpp src/SharedMemoryTransportV2.cpp src/UdpTransport.cpp"

# 编译进程内测试
echo -e "  编译 test_inprocess..."
g++ $COMMON_FLAGS -o test_inprocess test_inprocess.cpp $SOURCES
echo -e "${GREEN}  ✓ test_inprocess${NC}"

# 编译进程间测试
echo -e "  编译 test_interprocess_receiver..."
g++ $COMMON_FLAGS -o test_interprocess_receiver test_interprocess_receiver.cpp $SOURCES
echo -e "${GREEN}  ✓ test_interprocess_receiver${NC}"

echo -e "  编译 test_interprocess_sender..."
g++ $COMMON_FLAGS -o test_interprocess_sender test_interprocess_sender.cpp $SOURCES
echo -e "${GREEN}  ✓ test_interprocess_sender${NC}"

echo -e "${GREEN}✓ 所有程序编译完成${NC}\n"

# 运行进程内测试
echo -e "${YELLOW}[3/4] 运行进程内测试...${NC}"
echo ""

rm -f /dev/shm/librpc_shm_v2
./test_inprocess all

echo ""

# 运行进程间测试
echo -e "${YELLOW}[4/4] 运行进程间测试...${NC}"
echo ""

# 清理可能存在的旧进程
killall -9 test_interprocess_receiver 2>/dev/null || true
rm -f /dev/shm/librpc_shm_v2

# 启动接收端（后台运行）
echo -e "${BLUE}启动接收端进程...${NC}"
./test_interprocess_receiver &
RECEIVER_PID=$!

# 等待接收端启动
sleep 1

# 启动发送端
echo -e "${BLUE}启动发送端进程...${NC}"
./test_interprocess_sender 10000

# 等待接收端处理完
sleep 2

# 终止接收端
echo -e "${BLUE}终止接收端进程...${NC}"
kill -INT $RECEIVER_PID 2>/dev/null || true
wait $RECEIVER_PID 2>/dev/null || true

echo ""
echo -e "${GREEN}╔════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  所有测试完成!                       ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════╝${NC}"
