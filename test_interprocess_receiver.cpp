// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 进程间通信测试 - 接收端

#include "Node.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <signal.h>

using namespace librpc;

std::atomic<bool> running{true};
std::atomic<int> received_count{0};

void signalHandler(int signum) {
    running = false;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::string node_id = "receiver";
    if (argc > 1) {
        node_id = argv[1];
    }
    
    std::cout << "[接收端] 启动节点: " << node_id << "\n";
    
    auto node = createNode(node_id, TransportMode::LOCK_FREE_SHM);
    if (!node) {
        std::cerr << "❌ 创建节点失败\n";
        return 1;
    }
    
    auto callback = [&](const std::string& group, const std::string& topic,
                        const uint8_t* data, size_t size) {
        received_count++;
        if (received_count % 1000 == 0) {
            std::cout << "[接收端] 已接收 " << received_count << " 条消息\n";
        }
    };
    
    node->subscribe("test_group", {"topic1"}, callback);
    std::cout << "[接收端] 已订阅 test_group/topic1\n";
    std::cout << "[接收端] 等待消息... (Ctrl+C退出)\n";
    
    auto start = std::chrono::steady_clock::now();
    
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "\n[接收端] 统计:\n";
    std::cout << "  接收消息: " << received_count << " 条\n";
    std::cout << "  运行时间: " << duration.count() / 1000.0 << " 秒\n";
    if (duration.count() > 0) {
        std::cout << "  平均速率: " << (received_count * 1000.0 / duration.count()) << " msg/s\n";
    }
    
    return 0;
}
