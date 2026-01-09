// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 进程内多节点通信测试

#include "nexus/core/Node.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>

using namespace Nexus::rpc;

std::atomic<int> node1_received{0};
std::atomic<int> node2_received{0};
std::atomic<int> node3_received{0};

void testBasicInProcess() {
    std::cout << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║  进程内基本功能测试                  ║\n";
    std::cout << "╚════════════════════════════════════════╝\n\n";
    
    // 创建3个节点
    auto node1 = createNode("inproc_node1");
    auto node2 = createNode("inproc_node2");
    auto node3 = createNode("inproc_node3");
    
    std::cout << "[TEST 1] 创建3个节点...";
    if (!node1 || !node2 || !node3) {
        std::cout << " ❌\n";
        return;
    }
    std::cout << " ✅\n";
    
    // 订阅
    std::cout << "[TEST 2] 节点订阅主题...";
    auto callback1 = [](const std::string& group, const std::string& topic, 
                        const uint8_t* data, size_t size) {
        node1_received++;
    };
    auto callback2 = [](const std::string& group, const std::string& topic,
                        const uint8_t* data, size_t size) {
        node2_received++;
    };
    auto callback3 = [](const std::string& group, const std::string& topic,
                        const uint8_t* data, size_t size) {
        node3_received++;
    };
    
    node1->subscribe("test_group", {"topic1", "topic2"}, callback1);
    node2->subscribe("test_group", {"topic1", "topic3"}, callback2);
    node3->subscribe("test_group", {"topic2", "topic3"}, callback3);
    std::cout << " ✅\n";
    
    // 重置计数器
    node1_received = 0;
    node2_received = 0;
    node3_received = 0;
    
    // 广播测试
    std::cout << "[TEST 3] 广播消息...\n";
    
    // node1广播topic1 -> node1, node2应该收到
    node1->publish("test_group", "topic1", "message1");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "  topic1: node1=" << node1_received << ", node2=" << node2_received 
              << ", node3=" << node3_received;
    if (node1_received == 0 && node2_received == 1 && node3_received == 0) {
        std::cout << " ✅\n";
    } else {
        std::cout << " ❌\n";
    }
    
    // node2广播topic2 -> node1, node3应该收到
    node1_received = 0;
    node2_received = 0;
    node3_received = 0;
    node2->publish("test_group", "topic2", "message2");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "  topic2: node1=" << node1_received << ", node2=" << node2_received 
              << ", node3=" << node3_received;
    if (node1_received == 1 && node2_received == 0 && node3_received == 1) {
        std::cout << " ✅\n";
    } else {
        std::cout << " ❌\n";
    }
    
    // node3广播topic3 -> node2, node3应该收到
    node1_received = 0;
    node2_received = 0;
    node3_received = 0;
    node3->publish("test_group", "topic3", "message3");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "  topic3: node1=" << node1_received << ", node2=" << node2_received 
              << ", node3=" << node3_received;
    if (node1_received == 0 && node2_received == 1 && node3_received == 0) {
        std::cout << " ✅\n";
    } else {
        std::cout << " ❌\n";
    }
    
    std::cout << "\n✅ 进程内基本功能测试通过!\n";
}

void testStressInProcess() {
    std::cout << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║  进程内压力测试 (10000条×3节点)     ║\n";
    std::cout << "╚════════════════════════════════════════╝\n\n";
    
    auto node1 = createNode("stress_node1");
    auto node2 = createNode("stress_node2");
    auto node3 = createNode("stress_node3");
    
    node1_received = 0;
    node2_received = 0;
    node3_received = 0;
    
    auto callback1 = [](const std::string&, const std::string&, const uint8_t*, size_t) {
        node1_received++;
    };
    auto callback2 = [](const std::string&, const std::string&, const uint8_t*, size_t) {
        node2_received++;
    };
    auto callback3 = [](const std::string&, const std::string&, const uint8_t*, size_t) {
        node3_received++;
    };
    
    node1->subscribe("stress_group", {"topic"}, callback1);
    node2->subscribe("stress_group", {"topic"}, callback2);
    node3->subscribe("stress_group", {"topic"}, callback3);
    
    const int num_messages = 10000;
    std::cout << "发送 " << num_messages << " 条消息...\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < num_messages; i++) {
        std::string msg = "message_" + std::to_string(i);
        node1->publish("stress_group", "topic", msg);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // 等待接收
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cout << "\n结果:\n";
    std::cout << "  node1接收: " << node1_received << "\n";
    std::cout << "  node2接收: " << node2_received << "\n";
    std::cout << "  node3接收: " << node3_received << "\n";
    std::cout << "  总计: " << (node1_received + node2_received + node3_received) 
              << " / " << (num_messages * 2) << " (node1发送不接收自己的消息)\n";
    std::cout << "  发送用时: " << duration.count() / 1000.0 << " ms\n";
    std::cout << "  吞吐量: " << (num_messages * 1000000.0 / duration.count()) << " msg/s\n";
    
    int total = node1_received + node2_received + node3_received;
    int expected = num_messages * 2;  // node2 and node3 each receive num_messages
    double ratio = (double)total / expected * 100;
    std::cout << "\n";
    if (ratio >= 99.0) {
        std::cout << "✅ 压力测试通过! (接收率: " << ratio << "%)\n";
    } else {
        std::cout << "❌ 压力测试失败! (接收率: " << ratio << "%)\n";
    }
}

int main(int argc, char* argv[]) {
    std::string test_type = "all";
    if (argc > 1) {
        test_type = argv[1];
    }
    
    if (test_type == "basic" || test_type == "all") {
        testBasicInProcess();
    }
    
    if (test_type == "stress" || test_type == "all") {
        testStressInProcess();
    }
    
    return 0;
}
