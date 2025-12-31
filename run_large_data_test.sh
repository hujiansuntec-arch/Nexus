#!/bin/bash
# 大数据通道性能测试脚本

set -e

cd "$(dirname "$0")"

# 设置库路径
export LD_LIBRARY_PATH=./build:$LD_LIBRARY_PATH
export NEXUS_LOG_LEVEL=DEBUG  # 使用DEBUG级别查看详细连接信息

# 清理旧的共享内存
echo "清理环境..."
# rm -f /dev/shm/librpc_* /dev/shm/test_channel
killall -9 test_large_receiver 2>/dev/null || true
sleep 1

echo "======================================"
echo "大数据通道性能测试"
echo "======================================"
echo ""

# 测试场景
declare -a tests=(
    "10:512:测试1: 10次 × 512KB"
    "50:1024:测试2: 50次 × 1MB"
    "100:1024:测试3: 100次 × 1MB (高频)"
    "50:2048:测试4: 50次 × 2MB"
    "20:4096:测试5: 20次 × 4MB"
)

# 启动接收端
    ./build/test_large_receiver > ./tmp/receiver.log 2>&1 &
    RECEIVER_PID=$!
    sleep 1

for test in "${tests[@]}"; do
    IFS=':' read -r count size desc <<< "$test"
    
    echo "========== $desc =========="
    echo "参数: ${count}次, ${size}KB/次"
    echo ""
    
    # 运行发送端
    ./build/test_large_sender $count $size 2>&1 | tee ./tmp/sender.log | grep -E "(吞吐量|平均速度|耗时|成功)"
    
    echo ""
    
    # 停止接收端
    # kill -INT $RECEIVER_PID 2>/dev/null || true
    sleep 1
    
    # 显示接收端统计
    cat ./tmp/receiver.log | grep -A 10 "接收统计" | head -12
    
    echo ""
    echo "--------------------------------------"
    echo ""
    
    # 清理
    # rm -f /dev/shm/test_channel
    sleep 1
done

echo "======================================"
echo "测试完成"
echo "======================================"
echo ""

# 检查内存占用
echo "共享内存使用情况:"
ls -lh /dev/shm/librpc_* 2>/dev/null || echo "  (已清理)"
