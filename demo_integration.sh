#!/bin/bash

# LargeDataChannel 集成演示脚本
# 展示通过统一Node接口使用大数据传输功能

echo "========================================"
echo "  LargeDataChannel 集成演示"
echo "========================================"
echo ""
echo "特性："
echo "  ✅ 统一Node接口 - node->getLargeDataChannel()"
echo "  ✅ 自动配置优化 - 64MB + MAP_NORESERVE"
echo "  ✅ 零拷贝架构 - ~390 MB/s吞吐量"
echo "  ✅ 数据完整性 - CRC32校验"
echo ""

# 清理环境
echo "清理共享内存..."
rm -f /dev/shm/librpc_* /dev/shm/test_channel
sleep 1

# 启动接收端（后台）
echo "启动接收端..."
LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH ./test_large_receiver > /tmp/receiver.log 2>&1 &
RECEIVER_PID=$!
echo "  接收端 PID: $RECEIVER_PID"
sleep 2

# 检查接收端是否成功启动
if ! ps -p $RECEIVER_PID > /dev/null; then
    echo "  ❌ 接收端启动失败"
    exit 1
fi
echo "  ✅ 接收端就绪"
echo ""

# 运行发送端测试
echo "========================================"
echo "  演示1: 小数据量 (10×512KB)"
echo "========================================"
LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH ./test_large_sender 10 512
echo ""
sleep 1

echo "========================================"
echo "  演示2: 中等数据量 (30×1MB)"
echo "========================================"
LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH ./test_large_sender 30 1024
echo ""
sleep 1

echo "========================================"
echo "  演示3: 大数据块 (10×2MB)"
echo "========================================"
LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH ./test_large_sender 10 2048
echo ""

# 停止接收端并显示统计
echo "停止接收端..."
kill -INT $RECEIVER_PID
sleep 2

echo ""
echo "========================================"
echo "  接收端统计信息"
echo "========================================"
tail -20 /tmp/receiver.log | grep -A 20 "接收统计"

echo ""
echo "========================================"
echo "  演示完成！"
echo "========================================"
echo ""
echo "关键优势："
echo "  1. 简化API - 无需手动创建LargeDataChannel"
echo "  2. 自动管理 - 通道由Node统一管理"
echo "  3. 高性能  - 保持~390 MB/s吞吐量"
echo "  4. 零配置 - 自动应用最优配置"
echo ""
echo "完整文档："
echo "  - INTEGRATION_COMPLETE.md - 集成总结"
echo "  - HIGH_FREQUENCY_LARGE_DATA_SOLUTION.md - 技术方案"
echo ""
