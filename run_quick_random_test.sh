#!/bin/bash
# 快速随机退出测试 - 用于快速验证稳定性
# 使用较短的时间和较少的节点

cd /home/fz296w/workspace/Nexus/Nexus

# 清理环境
killall -9 test_duplex_v2 2>/dev/null || true
rm -f /dev/shm/librpc_* 2>/dev/null || true

echo "======================================"
echo "快速随机退出测试"
echo "======================================"
echo "测试时长: 30秒"
echo "节点数: 4个"
echo "存活时间: 3-8秒"
echo "======================================"
echo ""

./run_random_exit_test.sh 70 4 10 60 256 10000

echo ""
echo "快速测试完成！"
