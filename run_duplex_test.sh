#!/bin/bash
# 全双工测试启动脚本

set -e

cd /home/fz296w/workspace/polaris_rpc_qnx/librpc

# 清理环境
echo "清理环境..."
killall -9 test_duplex_v2 2>/dev/null || true
rm -f /dev/shm/librpc_*

# 设置参数
DURATION=${1:-20}
MSG_SIZE=${2:-256}
RATE=${3:-1000}

echo "======================================"
echo "全双工通信测试"
echo "======================================"
echo "持续时间: ${DURATION}秒"
echo "消息大小: ${MSG_SIZE}字节"
echo "发送速率: ${RATE} msg/s (间隔: $((1000000/RATE))μs)"
echo "======================================"
echo ""

# 设置库路径
export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH

# 同时启动两个节点（后台）
echo "启动 node0..."
./test_duplex_v2 node0 node1 $DURATION $MSG_SIZE $RATE > /tmp/node0.log 2>&1 &
PID0=$!

echo "启动 node1..."
./test_duplex_v2 node1 node0 $DURATION $MSG_SIZE $RATE > /tmp/node1.log 2>&1 &
PID1=$!

echo ""
echo "等待测试完成..."
echo "node0 PID: $PID0"
echo "node1 PID: $PID1"
echo ""

# 等待两个进程完成
wait $PID0
wait $PID1

echo ""
echo "======================================"
echo "测试完成！"
echo "======================================"
echo ""

# 显示结果
echo "========== Node0 结果 =========="
cat /tmp/node0.log | grep -A 10 "最终统计" || echo "node0日志异常"

echo ""
echo "========== Node1 结果 =========="
cat /tmp/node1.log | grep -A 10 "最终统计" || echo "node1日志异常"

# 清理
rm -f /tmp/node0.log /tmp/node1.log
