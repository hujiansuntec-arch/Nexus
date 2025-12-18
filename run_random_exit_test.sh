#!/bin/bash
# 随机节点退出稳定性测试脚本
# 验证框架在节点随机退出/重启情况下的稳定性和恢复能力

set -e

cd /home/fz296w/workspace/Nexus/Nexus

# 清理环境
echo "清理环境..."
killall -9 test_duplex_v2 2>/dev/null || true
rm -f ./tmp/random_exit_*.log

# 设置参数
TOTAL_DURATION=${1:-60}      # 总测试时长（秒）
NUM_NODES=${2:-8}             # 节点总数
MIN_LIFETIME=${3:-5}          # 节点最小存活时间（秒）
MAX_LIFETIME=${4:-15}         # 节点最大存活时间（秒）
MSG_SIZE=${5:-256}            # 消息大小（字节）
RATE=${6:-500}                # 发送速率（msg/s）

echo "======================================"
echo "节点随机退出稳定性测试"
echo "======================================"
echo "总测试时长: ${TOTAL_DURATION}秒"
echo "节点总数: ${NUM_NODES} 个"
echo "节点存活时间: ${MIN_LIFETIME}-${MAX_LIFETIME}秒 (随机)"
echo "消息大小: ${MSG_SIZE}字节"
echo "发送速率: ${RATE} msg/s"
echo "======================================"
echo ""

# 设置库路径和日志级别
export LD_LIBRARY_PATH=./build:$LD_LIBRARY_PATH
export NEXUS_LOG_LEVEL=INFO

# 创建日志目录
mkdir -p ./tmp

# 节点状态跟踪
declare -a NODE_PIDS        # 当前运行的节点PIDs
declare -a NODE_NAMES       # 节点名称
declare -a NODE_START_TIME  # 节点启动时间
declare -a NODE_LIFETIME    # 节点预期存活时间
declare -a NODE_RESTART_COUNT  # 节点重启次数
declare -a NODE_TOTAL_RUNTIME  # 节点累计运行时间

# 初始化节点数组
for ((i=0; i<NUM_NODES; i++)); do
    NODE_NAMES[$i]="node$i"
    NODE_PIDS[$i]=0
    NODE_START_TIME[$i]=0
    NODE_LIFETIME[$i]=0
    NODE_RESTART_COUNT[$i]=0
    NODE_TOTAL_RUNTIME[$i]=0
done

# 获取当前时间戳（秒）
get_timestamp() {
    date +%s
}

# 生成随机存活时间
random_lifetime() {
    echo $((RANDOM % (MAX_LIFETIME - MIN_LIFETIME + 1) + MIN_LIFETIME))
}

# 选择一个随机的目标节点（不包括自己）
get_random_target() {
    local current=$1
    local target=$((RANDOM % NUM_NODES))
    while [ $target -eq $current ]; do
        target=$((RANDOM % NUM_NODES))
    done
    echo $target
}

# 启动单个节点
start_node() {
    local idx=$1
    local node_name="${NODE_NAMES[$idx]}"
    local target_idx=$(get_random_target $idx)
    local target_name="${NODE_NAMES[$target_idx]}"
    local lifetime=$(random_lifetime)
    
    # 节点运行固定时长后自动退出
    local duration=$lifetime
    
    echo "[$(date '+%H:%M:%S')] 启动 $node_name → $target_name (预期存活: ${lifetime}秒)"
    
    ./build/test_duplex_v2 "$node_name" "$target_name" $duration $MSG_SIZE $RATE \
        > "./tmp/random_exit_${node_name}_${NODE_RESTART_COUNT[$idx]}.log" 2>&1 &
    
    NODE_PIDS[$idx]=$!
    NODE_START_TIME[$idx]=$(get_timestamp)
    NODE_LIFETIME[$idx]=$lifetime
    NODE_RESTART_COUNT[$idx]=$((${NODE_RESTART_COUNT[$idx]} + 1))
}

# 检查节点状态并处理退出的节点
check_and_restart_nodes() {
    local current_time=$(get_timestamp)
    
    for ((i=0; i<NUM_NODES; i++)); do
        local pid=${NODE_PIDS[$i]}
        
        # 跳过未启动的节点
        if [ $pid -eq 0 ]; then
            continue
        fi
        
        # 检查进程是否还在运行
        if ! kill -0 $pid 2>/dev/null; then
            local runtime=$((current_time - ${NODE_START_TIME[$i]}))
            local total_runtime=$((${NODE_TOTAL_RUNTIME[$i]} + runtime))
            NODE_TOTAL_RUNTIME[$i]=$total_runtime
            
            echo "[$(date '+%H:%M:%S')] ${NODE_NAMES[$i]} 已退出 (运行: ${runtime}秒, 累计: ${total_runtime}秒, 重启次数: ${NODE_RESTART_COUNT[$i]})"
            
            # 标记为已退出
            NODE_PIDS[$i]=0
            
            # 延迟随机时间再重启（模拟真实场景）
            local restart_delay=$((RANDOM % 3 + 1))
            echo "[$(date '+%H:%M:%S')] ${NODE_NAMES[$i]} 将在 ${restart_delay}秒后重启"
            sleep $restart_delay
            
            # 重新启动节点
            start_node $i
        fi
    done
}

# 停止所有节点
stop_all_nodes() {
    echo ""
    echo "正在停止所有节点..."
    # for ((i=0; i<NUM_NODES; i++)); do
    #     local pid=${NODE_PIDS[$i]}
    #     if [ $pid -ne 0 ] && kill -0 $pid 2>/dev/null; then
    #         kill -TERM $pid 2>/dev/null || true
            
    #         # 更新运行时间统计
    #         local current_time=$(get_timestamp)
    #         local runtime=$((current_time - ${NODE_START_TIME[$i]}))
    #         NODE_TOTAL_RUNTIME[$i]=$((${NODE_TOTAL_RUNTIME[$i]} + runtime))
    #     fi
    # done
    
    # # 等待所有进程退出
    # sleep 2
    # killall -9 test_duplex_v2 2>/dev/null || true
}

# 主测试循环
echo "开始测试..."
echo ""

# 初始化：启动所有节点
for ((i=0; i<NUM_NODES; i++)); do
    start_node $i
    sleep 0.2  # 错开启动时间
done

echo ""
echo "所有节点已启动，开始监控..."
echo ""

# 测试开始时间
TEST_START=$(get_timestamp)

# 主监控循环
while true; do
    CURRENT_TIME=$(get_timestamp)
    ELAPSED=$((CURRENT_TIME - TEST_START))
    
    # 检查是否达到总测试时长
    if [ $ELAPSED -ge $TOTAL_DURATION ]; then
        echo ""
        echo "======================================"
        echo "测试时长已到 (${ELAPSED}秒)，结束测试"
        echo "======================================"
        break
    fi
    
    # 每秒检查一次节点状态
    check_and_restart_nodes
    sleep 1
done

# 停止所有节点
stop_all_nodes

echo ""
echo "======================================"
echo "测试完成！生成统计报告..."
echo "======================================"
echo ""

# 统计分析
echo "======================================"
echo "节点运行统计"
echo "======================================"
printf "%-10s %8s %12s %12s\n" "节点" "重启次数" "累计运行(秒)" "运行率"

for ((i=0; i<NUM_NODES; i++)); do
    local node_name="${NODE_NAMES[$i]}"
    local restart_count=${NODE_RESTART_COUNT[$i]}
    local total_runtime=${NODE_TOTAL_RUNTIME[$i]}
    local runtime_ratio=$((total_runtime * 100 / TOTAL_DURATION))
    
    printf "%-10s %8d %12d %11d%%\n" "$node_name" $restart_count $total_runtime $runtime_ratio
done

echo ""
echo "======================================"
echo "消息传输统计"
echo "======================================"

total_sent=0
total_received=0
total_lost=0

for ((i=0; i<NUM_NODES; i++)); do
    local node_name="${NODE_NAMES[$i]}"
    
    # 合并所有重启周期的日志
    local sent=0
    local received=0
    
    for logfile in ./tmp/random_exit_${node_name}_*.log; do
        if [ -f "$logfile" ]; then
            # 提取发送和接收数量
            local s=$(grep "发送消息:" "$logfile" | tail -1 | sed 's/.*发送消息: //' | sed 's/,.*//' || echo 0)
            local r=$(grep "接收消息:" "$logfile" | tail -1 | sed 's/.*接收消息: //' | sed 's/,.*//' || echo 0)
            
            sent=$((sent + s))
            received=$((received + r))
        fi
    done
    
    local lost=$((sent - received))
    if [ $sent -gt 0 ]; then
        local loss_rate=$((lost * 100 / sent))
    else
        local loss_rate=0
    fi
    
    printf "%-10s 发送: %8d  接收: %8d  丢失: %8d  丢包率: %3d%%\n" \
        "[$node_name]" $sent $received $lost $loss_rate
    
    total_sent=$((total_sent + sent))
    total_received=$((total_received + received))
    total_lost=$((total_lost + lost))
done

echo ""
echo "======================================"
echo "汇总统计"
echo "======================================"
echo "总测试时长: ${TOTAL_DURATION}秒"
echo "节点总数: ${NUM_NODES}"

# 计算平均重启次数
total_restarts=0
for ((i=0; i<NUM_NODES; i++)); do
    total_restarts=$((total_restarts + ${NODE_RESTART_COUNT[$i]}))
done
avg_restarts=$((total_restarts / NUM_NODES))
echo "总重启次数: ${total_restarts} (平均: ${avg_restarts}次/节点)"

# 消息统计
if [ $total_sent -gt 0 ]; then
    total_loss_rate=$((total_lost * 100 / total_sent))
else
    total_loss_rate=0
fi

echo ""
echo "消息统计:"
echo "  总发送: ${total_sent}"
echo "  总接收: ${total_received}"
echo "  总丢失: ${total_lost}"
echo "  总丢包率: ${total_loss_rate}%"

echo ""
echo "======================================"
echo "框架稳定性评估"
echo "======================================"

# 评估稳定性
if [ $total_loss_rate -lt 5 ]; then
    echo "✓ 稳定性: 优秀 (丢包率 < 5%)"
elif [ $total_loss_rate -lt 15 ]; then
    echo "○ 稳定性: 良好 (丢包率 < 15%)"
elif [ $total_loss_rate -lt 30 ]; then
    echo "△ 稳定性: 一般 (丢包率 < 30%)"
else
    echo "✗ 稳定性: 较差 (丢包率 >= 30%)"
fi

# 检查是否有节点崩溃（重启次数异常少）
min_expected_restarts=$((TOTAL_DURATION / MAX_LIFETIME - 1))
crashed_nodes=0
for ((i=0; i<NUM_NODES; i++)); do
    if [ ${NODE_RESTART_COUNT[$i]} -lt $min_expected_restarts ]; then
        if [ $crashed_nodes -eq 0 ]; then
            echo ""
            echo "⚠ 可能崩溃的节点 (重启次数异常):"
        fi
        echo "  - ${NODE_NAMES[$i]}: 仅重启 ${NODE_RESTART_COUNT[$i]} 次"
        crashed_nodes=$((crashed_nodes + 1))
    fi
done

if [ $crashed_nodes -eq 0 ]; then
    echo "✓ 所有节点正常运行，无崩溃"
fi

echo ""
echo "详细日志保存在: ./tmp/random_exit_*.log"
echo ""

echo "测试完成！"
