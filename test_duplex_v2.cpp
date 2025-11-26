// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 改进的全双工通信性能测试 - 带流控和生命周期管理

#include "Node.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <signal.h>
#include <cstring>
#include <memory>

using namespace librpc;

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

class DuplexWorker {
public:
    DuplexWorker(const std::string& id, const std::string& peer_id, 
                 int duration, int msg_size, int target_rate)
        : node_id_(id)
        , peer_id_(peer_id)
        , duration_sec_(duration)
        , msg_size_(msg_size)
        , target_rate_(target_rate)
        , sent_(0)
        , received_(0)
        , echoed_(0) {
        
        my_topic_ = "to_" + node_id_;
        peer_topic_ = "to_" + peer_id_;
    }
    
    ~DuplexWorker() {
        std::cout << "[" << node_id_ << "] 析构中...\n";
        if (node_) {
            // 先停止接收线程
            g_running = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            node_.reset();
        }
    }
    
    void run() {
        std::cout << "\n╔════════════════════════════════════════╗\n";
        std::cout << "║  [" << node_id_ << "] 全双工性能测试          ║\n";
        std::cout << "╠════════════════════════════════════════╣\n";
        std::cout << "║  对端: " << peer_id_ << "\n";
        std::cout << "║  消息大小: " << msg_size_ << " 字节\n";
        std::cout << "║  持续时间: " << duration_sec_ << " 秒\n";
        std::cout << "║  目标速率: " << target_rate_ << " msg/s\n";
        std::cout << "╚════════════════════════════════════════╝\n\n";
        
        // 创建节点
        node_ = createNode(node_id_);
        
        // 订阅回复消息
        auto callback = [this](const std::string&, const std::string&,
                              const uint8_t* data, size_t size) {
            received_.fetch_add(1, std::memory_order_relaxed);
            
            // Echo back
            // if (size >= 16) {
            //     char sender[16] = {0};
            //     std::memcpy(sender, data, std::min(size_t(15), size));
                
            //     // 只回复给发送者
            //     if (std::string(sender) == peer_id_) {
            //         node_->broadcast("duplex", peer_topic_,
            //                        std::string((char*)data, size));
            //         echoed_.fetch_add(1, std::memory_order_relaxed);
            //     }
            // }
        };
        
        node_->subscribe("duplex", {my_topic_}, callback);
        
        std::cout << "[" << node_id_ << "] 等待连接...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        // 准备消息
        std::vector<uint8_t> payload(msg_size_);
        std::memcpy(payload.data(), node_id_.c_str(),
                   std::min(node_id_.size(), size_t(15)));
        
        // 计算发送间隔（微秒）
        int send_interval_us = target_rate_ > 0 ? 
            (1000000 / target_rate_) : 0;
        
        std::cout << "[" << node_id_ << "] 开始发送 (间隔:" 
                  << send_interval_us << "μs)...\n\n";
        
        auto start = std::chrono::steady_clock::now();
        auto last_print = start;
        
        while (g_running) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - start).count();
            
            if (elapsed >= duration_sec_) {
                break;
            }
            
            // 发送消息
            node_->publish("duplex", peer_topic_,
                           std::string((char*)payload.data(), payload.size()));
            sent_.fetch_add(1, std::memory_order_relaxed);
            
            // 流控：按目标速率休息
            if (send_interval_us > 0) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(send_interval_us));
            }
            
            // 每秒打印一次进度
            auto since_print = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_print).count();
            if (since_print >= 1) {
                printProgress(elapsed);
                last_print = now;
            }
        }
        
        std::cout << "\n[" << node_id_ << "] 发送完成，等待接收...\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        printFinalStats(start);
    }
    
private:
    void printProgress(int elapsed) {
        std::cout << "[" << node_id_ << "] " << elapsed << "s"
                  << " | 发送:" << sent_.load()
                  << " | 接收:" << received_.load()
                  << " | 回显:" << echoed_.load()
                  << "\r" << std::flush;
    }
    
    void printFinalStats(std::chrono::steady_clock::time_point start) {
        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
        
        uint64_t sent = sent_.load();
        uint64_t received = received_.load();
        uint64_t echoed = echoed_.load();
        
        std::cout << "\n\n╔════════════════════════════════════════╗\n";
        std::cout << "║  [" << node_id_ << "] 最终统计 (" << ms << "ms)      ║\n";
        std::cout << "╠════════════════════════════════════════╣\n";
        std::cout << "║  发送消息: " << sent << "\n";
        std::cout << "║  接收消息: " << received << "\n";
        std::cout << "║  回显消息: " << echoed << "\n";
        
        if (ms > 0) {
            double send_rate = (sent * 1000.0) / ms;
            double recv_rate = (received * 1000.0) / ms;
            double send_mbps = (sent * msg_size_ * 8.0 / 1000000) / (ms / 1000.0);
            double recv_mbps = (received * msg_size_ * 8.0 / 1000000) / (ms / 1000.0);
            
            std::cout << "║  \n";
            std::cout << "║  发送速率: " << (int)send_rate << " msg/s"
                      << " (" << send_mbps << " Mbps)\n";
            std::cout << "║  接收速率: " << (int)recv_rate << " msg/s"
                      << " (" << recv_mbps << " Mbps)\n";
            
            if (sent > 0) {
                double loss_rate = 100.0 * (1.0 - (double)received / sent);
                std::cout << "║  丢包率: " << loss_rate << "%\n";
            }
        }
        
        std::cout << "╚════════════════════════════════════════╝\n";
    }
    
private:
    std::string node_id_;
    std::string peer_id_;
    std::string my_topic_;
    std::string peer_topic_;
    int duration_sec_;
    int msg_size_;
    int target_rate_;
    
    std::shared_ptr<Node> node_;
    std::atomic<uint64_t> sent_;
    std::atomic<uint64_t> received_;
    std::atomic<uint64_t> echoed_;
};

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    if (argc < 6) {
        std::cout << "用法: " << argv[0] 
                  << " <node_id> <peer_id> <duration_sec> <msg_size> <rate_per_sec>\n\n";
        std::cout << "参数说明:\n";
        std::cout << "  node_id      - 本节点ID (如: node0)\n";
        std::cout << "  peer_id      - 对端节点ID (如: node1)\n";
        std::cout << "  duration_sec - 测试时长(秒)\n";
        std::cout << "  msg_size     - 消息大小(字节)\n";
        std::cout << "  rate_per_sec - 目标发送速率(msg/s)\n\n";
        std::cout << "示例 - 稳定性测试 (1万msg/s):\n";
        std::cout << "  终端1: " << argv[0] << " node0 node1 60 256 10000\n";
        std::cout << "  终端2: " << argv[0] << " node1 node0 60 256 10000\n\n";
        std::cout << "示例 - 中等压力 (10万msg/s):\n";
        std::cout << "  终端1: " << argv[0] << " node0 node1 30 256 100000\n";
        std::cout << "  终端2: " << argv[0] << " node1 node0 30 256 100000\n\n";
        std::cout << "示例 - 高压力 (50万msg/s):\n";
        std::cout << "  终端1: " << argv[0] << " node0 node1 10 256 500000\n";
        std::cout << "  终端2: " << argv[0] << " node1 node0 10 256 500000\n\n";
        return 1;
    }
    
    std::string node_id = argv[1];
    std::string peer_id = argv[2];
    int duration = std::atoi(argv[3]);
    int msg_size = std::atoi(argv[4]);
    int rate = std::atoi(argv[5]);
    
    {
        DuplexWorker worker(node_id, peer_id, duration, msg_size, rate);
        worker.run();
    }
    
    std::cout << "\n[" << node_id << "] 程序正常退出\n";
    return 0;
}
