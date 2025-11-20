// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 进程间通信测试 - 发送端

#include "Node.h"
#include <iostream>
#include <chrono>
#include <thread>

using namespace librpc;

int main(int argc, char* argv[]) {
    int num_messages = 10000;
    if (argc > 1) {
        num_messages = std::atoi(argv[1]);
    }
    
    std::cout << "[发送端] 启动节点: sender\n";
    
    auto node = createNode("sender", TransportMode::LOCK_FREE_SHM);
    if (!node) {
        std::cerr << "❌ 创建节点失败\n";
        return 1;
    }
    
    // 等待接收端准备好
    std::cout << "[发送端] 等待2秒让接收端准备...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    std::cout << "[发送端] 开始发送 " << num_messages << " 条消息...\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_messages; i++) {
        std::string msg = "message_" + std::to_string(i);
        node->broadcast("test_group", "topic1", msg);
        
        if ((i + 1) % 1000 == 0) {
            std::cout << "[发送端] 已发送 " << (i + 1) << " 条消息\n";
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "\n[发送端] 发送完成!\n";
    std::cout << "  发送消息: " << num_messages << " 条\n";
    std::cout << "  发送用时: " << duration.count() / 1000.0 << " ms\n";
    std::cout << "  发送速率: " << (num_messages * 1000000.0 / duration.count()) << " msg/s\n";
    
    // 等待一段时间让接收端处理完
    std::cout << "\n[发送端] 等待1秒让接收端处理完...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    return 0;
}
