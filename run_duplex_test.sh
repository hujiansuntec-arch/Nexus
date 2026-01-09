#!/bin/bash
# 全双工测试启动脚本 - 支持P2P和Multi模式

set -e

cd /home/fz296w/workspace/Nexus/Nexus

# 显示用法
show_usage() {
    cat << EOF
用法: $0 [mode] [options]

模式1 - P2P模式（点对点通信，跨进程）:
  $0 p2p [duration] [msg_size] [rate] [num_pairs]
  
  参数:
    duration   - 测试持续时间（秒），默认: 20
    msg_size   - 消息大小（字节），默认: 256
    rate       - 发送速率（msg/s），默认: 1000
    num_pairs  - 节点对数，默认: 4 (共8个进程)
  
  示例:
    $0 p2p 10 128 500 2    # 2对节点(4进程)，10秒，128字节，500msg/s

模式2 - Multi模式（跨进程多节点，进程内+跨进程混合通信）:
  $0 multi [duration] [msg_size] [rate] [num_processes] [nodes_per_process] [topic]
  
  参数:
    duration          - 测试持续时间（秒），默认: 20
    msg_size          - 消息大小（字节），默认: 256
    rate              - 发送速率（msg/s），默认: 1000
    num_processes     - 进程数量，默认: 2
    nodes_per_process - 每进程节点数，默认: 4
    topic             - 订阅Topic，默认: shared_channel
  
  示例:
    $0 multi 10 128 500 2 4 my_topic  # 2进程×4节点，共享topic: my_topic
    $0 multi 10 128 500 3 2           # 3进程×2节点，默认topic
EOF
}

# 检查帮助参数
if [[ "$1" == "-h" ]] || [[ "$1" == "--help" ]]; then
    show_usage
    exit 0
fi

# 清理环境
echo "清理环境..."
killall -9 test_duplex_v2 2>/dev/null || true
rm -rf ./tmp
mkdir -p ./tmp

# 设置库路径和日志级别
export LD_LIBRARY_PATH=./build:$LD_LIBRARY_PATH
export NEXUS_LOG_LEVEL=INFO  # 使用INFO级别，避免过多日志

# 设置库路径和日志级别
export LD_LIBRARY_PATH=./build:$LD_LIBRARY_PATH
export NEXUS_LOG_LEVEL=INFO  # 使用INFO级别，避免过多日志

# 解析模式和参数
MODE=""
if [[ "$1" == "p2p" ]] || [[ "$1" == "multi" ]]; then
    MODE="$1"
    shift
fi

DURATION=${1:-20}
MSG_SIZE=${2:-256}
RATE=${3:-1000}

# P2P模式参数
if [ "$MODE" == "p2p" ] || [ -z "$MODE" ]; then
    NODE_COUNT=${4:-8}
    NUM_PAIRS=$((NODE_COUNT / 2))
# Multi模式参数  
elif [ "$MODE" == "multi" ]; then
    NUM_PROCESSES=${4:-2}
    NODES_PER_PROCESS=${5:-4}
    TOPIC=${6:-shared_channel}
fi

# 如果没有指定模式，默认使用P2P
if [ -z "$MODE" ]; then
    MODE="p2p"
fi

echo "======================================"
echo "全双工并发通信测试"
echo "======================================"
echo "测试模式: ${MODE^^}"
echo "持续时间: ${DURATION}秒"
echo "消息大小: ${MSG_SIZE}字节"
echo "发送速率: ${RATE} msg/s"

if [ "$MODE" == "multi" ]; then
    TOTAL_NODES=$((NUM_PROCESSES * NODES_PER_PROCESS))
    echo "进程数量: ${NUM_PROCESSES} 个"
    echo "每进程节点数: ${NODES_PER_PROCESS} 个"
    echo "总节点数: ${TOTAL_NODES} 个"
    echo "订阅Topic: ${TOPIC}"
    echo "通信拓扑: 进程内 + 跨进程混合通信"
else
    NUM_NODES=$((NUM_PAIRS * 2))
    echo "节点数量: ${NODE_COUNT} 个 (${NUM_PAIRS} 对跨进程)"
    echo "通信拓扑: P2P 点对点"
fi
echo "======================================"
echo ""

###########################################
# Multi模式：多进程，每进程多节点共享Topic
###########################################
if [ "$MODE" == "multi" ]; then
    echo "启动Multi模式测试..."
    echo "每个进程将创建 ${NODES_PER_PROCESS} 个节点，所有节点订阅: ${TOPIC}"
    echo ""
    
    declare -a MULTI_PIDS
    
    # 启动多个进程，每个进程运行多个节点
    for ((i=0; i<NUM_PROCESSES; i++)); do
        PROCESS_NAME="process${i}"
        BASE_NAME="p${i}_node"
        
        echo "  启动进程 [$PROCESS_NAME]: ${NODES_PER_PROCESS} 节点 (${BASE_NAME}_0 ~ ${BASE_NAME}_$((NODES_PER_PROCESS-1)))"
        
        # 每个进程运行test_duplex_v2 multi模式，传递topic参数
        stdbuf -oL ./build/test_duplex_v2 multi "$BASE_NAME" $NODES_PER_PROCESS $DURATION $MSG_SIZE $RATE "$TOPIC" \
            > ./tmp/${PROCESS_NAME}.log 2>&1 &
        
        MULTI_PIDS[$i]=$!
    done
    
    echo ""
    echo "所有进程已启动，进程PIDs: ${MULTI_PIDS[@]}"
    echo ""
    echo "提示: 使用 'tail -f ./tmp/process*.log' 查看实时日志"
    echo ""
    
    # 等待所有进程完成
    for pid in "${MULTI_PIDS[@]}"; do
        wait $pid 2>/dev/null || true
    done
    
    echo ""
    echo "======================================"
    echo "Multi模式测试完成！统计结果："
    echo "======================================"
    echo ""
    
    # 统计所有进程的结果
    total_sent=0
    total_recv=0
    success_processes=0
    failed_processes=0
    
    for ((i=0; i<NUM_PROCESSES; i++)); do
        PROCESS_NAME="process${i}"
        LOGFILE="./tmp/${PROCESS_NAME}.log"
        
        if [ -f "$LOGFILE" ]; then
            # 提取汇总统计 (使用-a强制视为文本)
            SENT=$(grep -a "总发送:" "$LOGFILE" | tail -1 | grep -oP '\d+' || echo "0")
            RECV=$(grep -a "总接收:" "$LOGFILE" | tail -1 | grep -oP '\d+' || echo "0")
            
            if [ -n "$SENT" ] && [ -n "$RECV" ] && [ "$SENT" -gt 0 ]; then
                total_sent=$((total_sent + SENT))
                total_recv=$((total_recv + RECV))
                
                # 计算放大倍率（应该接近 NODES_PER_PROCESS * NUM_PROCESSES - 1）
                RATIO=$(awk "BEGIN {printf \"%.2f\", $RECV / $SENT}")
                EXPECTED=$((NODES_PER_PROCESS * NUM_PROCESSES - 1))
                
                echo "[$PROCESS_NAME] 发送: $SENT | 接收: $RECV | 放大倍率: ${RATIO}x (预期: ~${EXPECTED}x)"
                success_processes=$((success_processes + 1))
            else
                echo "[$PROCESS_NAME] ❌ 日志异常 - 无统计数据"
                failed_processes=$((failed_processes + 1))
            fi
        else
            echo "[$PROCESS_NAME] ❌ 日志文件不存在"
            failed_processes=$((failed_processes + 1))
        fi
    done
    
    echo ""
    echo "======================================"
    echo "汇总统计"
    echo "======================================"
    echo "进程数量: ${NUM_PROCESSES}"
    echo "成功进程: ${success_processes}"
    echo "失败进程: ${failed_processes}"
    echo ""
    echo "总发送量: ${total_sent} 消息"
    echo "总接收量: ${total_recv} 消息"
    
    if [ "$total_sent" -gt 0 ]; then
        OVERALL_RATIO=$(awk "BEGIN {printf \"%.2f\", $total_recv / $total_sent}")
        echo "整体放大倍率: ${OVERALL_RATIO}x"
    fi
    
    echo ""
    
    # 判断测试是否成功
    if [ "$failed_processes" -eq 0 ] && [ "$success_processes" -eq "$NUM_PROCESSES" ]; then
        echo "✅ 测试成功：所有进程通信正常"
        TEST_RESULT=0
    else
        echo "❌ 测试失败：存在异常进程"
        TEST_RESULT=1
    fi
    
    echo ""
    echo "完整日志保存在: ./tmp/process*.log"
    echo ""
    
    exit $TEST_RESULT
fi

###########################################
# P2P模式：多进程点对点通信
###########################################
###########################################
# P2P模式：多进程点对点通信
###########################################
echo "启动P2P模式测试..."

# 配对方式: (node0, node1), (node2, node3), ...
NUM_NODES=$((NUM_PAIRS * 2))
declare -a PIDS

echo "正在启动 ${NUM_NODES} 个节点 (${NUM_PAIRS} 对)..."
for ((i=0; i<NUM_NODES; i+=2)); do
    NODE_A="node$i"
    NODE_B="node$((i+1))"
    
    echo "  启动节点对 [$NODE_A ↔ $NODE_B]"
    
    # 启动偶数节点 (发送给奇数节点)
    stdbuf -oL ./build/test_duplex_v2 $NODE_A $NODE_B $DURATION $MSG_SIZE $RATE > ./tmp/$NODE_A.log 2>&1 &
    PIDS[$i]=$!
    
    # 启动奇数节点 (发送给偶数节点)
    stdbuf -oL ./build/test_duplex_v2 $NODE_B $NODE_A $DURATION $MSG_SIZE $RATE > ./tmp/$NODE_B.log 2>&1 &
    PIDS[$((i+1))]=$!
done

echo ""
echo "所有节点已启动，等待测试完成..."
echo "进程PIDs: ${PIDS[@]}"
echo ""
echo "提示: 使用 'tail -f ./tmp/node*.log' 查看实时日志"
echo ""

# 等待所有进程完成
for pid in "${PIDS[@]}"; do
    wait $pid 2>/dev/null || true
done

echo ""
echo "======================================"
echo "P2P模式测试完成！统计结果："
echo "======================================"
echo ""

# 统计所有节点的发送/接收情况
success_count=0
fail_count=0
total_sent=0
total_recv=0
problem_nodes=""

for ((i=0; i<NUM_NODES; i++)); do
    NODE_NAME="node$i"
    LOGFILE="./tmp/$NODE_NAME.log"
    
    if [ -f "$LOGFILE" ]; then
        # 提取发送和接收数量 (使用-a强制视为文本)
        SENT=$(grep -a "发送消息:" "$LOGFILE" | tail -1 | grep -oP '\d+' || echo "0")
        RECV=$(grep -a "接收消息:" "$LOGFILE" | tail -1 | grep -oP '\d+' || echo "0")
        
        if [ -n "$SENT" ] && [ -n "$RECV" ]; then
            total_sent=$((total_sent + SENT))
            total_recv=$((total_recv + RECV))
            
            # 计算丢包率
            if [ "$SENT" -gt 0 ]; then
                LOSS=$(awk "BEGIN {printf \"%.2f\", (1 - $RECV / $SENT) * 100}")
            else
                LOSS="N/A"
            fi
            
            # 格式化输出
            printf "%-10s 发送: %8s | 接收: %8s | 丢包率: %8s%%\n" \
                "[$NODE_NAME]" "$SENT" "$RECV" "$LOSS"
            
            # 检查是否异常（丢包率 > 5%）
            if [ "$LOSS" != "N/A" ]; then
                LOSS_INT=$(echo "$LOSS" | cut -d. -f1)
                if [ "$LOSS_INT" -gt 5 ]; then
                    problem_nodes="$problem_nodes $NODE_NAME"
                    fail_count=$((fail_count + 1))
                else
                    success_count=$((success_count + 1))
                fi
            else
                fail_count=$((fail_count + 1))
            fi
        else
            echo "[$NODE_NAME] ❌ 日志异常 - 无统计数据"
            fail_count=$((fail_count + 1))
        fi
    else
        echo "[$NODE_NAME] ❌ 日志文件不存在"
        fail_count=$((fail_count + 1))
    fi
done

echo ""
echo "======================================"
echo "汇总统计"
echo "======================================"
echo "总节点数: ${NUM_NODES}"
echo "正常节点: ${success_count} (丢包率 <= 5%)"
echo "异常节点: ${fail_count}"
echo ""
echo "总发送量: ${total_sent} 消息"
echo "总接收量: ${total_recv} 消息"

if [ "$total_sent" -gt 0 ]; then
    OVERALL_LOSS=$(awk "BEGIN {printf \"%.2f\", (1 - $total_recv / $total_sent) * 100}")
    echo "整体丢包率: ${OVERALL_LOSS}%"
fi

if [ -n "$problem_nodes" ]; then
    echo ""
    echo "⚠️  异常节点:$problem_nodes"
fi
echo ""

# 判断测试是否成功
if [ "$fail_count" -eq 0 ] && [ "$success_count" -eq "$NUM_NODES" ]; then
    echo "✅ 测试成功：所有节点通信正常"
    TEST_RESULT=0
else
    echo "❌ 测试失败：存在通信异常节点"
    TEST_RESULT=1
fi

echo ""

# 显示详细结果（前4对节点）
echo "======================================"
echo "详细结果 (前4对节点)"
echo "======================================"
MAX_SHOW=$((NUM_NODES < 8 ? NUM_NODES : 8))
for ((i=0; i<MAX_SHOW; i++)); do
    NODE_NAME="node$i"
    if [ -f "./tmp/$NODE_NAME.log" ]; then
        echo ""
        echo "========== $NODE_NAME =========="
        grep -a -A 10 "最终统计" ./tmp/$NODE_NAME.log 2>/dev/null | head -15 || echo "[$NODE_NAME] 无详细统计"
    fi
done

echo ""
echo "完整日志保存在: ./tmp/node*.log"
echo ""

exit $TEST_RESULT