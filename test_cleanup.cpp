// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 测试自动清理功能

#include "SharedMemoryTransportV3.h"
#include <iostream>
#include <unistd.h>

using namespace librpc;

int main() {
    std::cout << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║  共享内存自动清理测试 (V3)           ║\n";
    std::cout << "╚════════════════════════════════════════╝\n\n";
    
    // 测试1: getActiveNodeCount (应该返回0)
    std::cout << "[测试1] 检查当前活跃节点数...\n";
    int count = SharedMemoryTransportV3::getActiveNodeCount();
    std::cout << "  活跃节点数: " << count << "\n";
    if (count == 0 || count == -1) {
        std::cout << "  ✅ 无活跃节点\n";
    } else {
        std::cout << "  ⚠️  有 " << count << " 个活跃节点\n";
    }
    
    // 测试2: 创建节点后检查
    std::cout << "\n[测试2] 创建一个节点...\n";
    {
        SharedMemoryTransportV3 transport;
        if (!transport.initialize("test_node")) {
            std::cerr << "  ❌ 初始化失败\n";
            return 1;
        }
        std::cout << "  ✅ 节点创建成功\n";
        
        count = SharedMemoryTransportV3::getActiveNodeCount();
        std::cout << "  活跃节点数: " << count << "\n";
        if (count == 1) {
            std::cout << "  ✅ 检测到1个活跃节点\n";
        } else {
            std::cout << "  ❌ 节点数不正确\n";
        }
    }
    // 节点在这里析构，应该自动清理共享内存
    
    std::cout << "\n[测试3] 节点析构后检查...\n";
    sleep(1);  // 等待一下
    count = SharedMemoryTransportV3::getActiveNodeCount();
    std::cout << "  活跃节点数: " << count << "\n";
    if (count == 0 || count == -1) {
        std::cout << "  ✅ 节点已自动清理\n";
    } else {
        std::cout << "  ❌ 节点未清理 (还有 " << count << " 个节点)\n";
    }
    
    // 测试4: 手动清理孤立内存
    std::cout << "\n[测试4] 测试手动清理功能...\n";
    if (SharedMemoryTransportV3::cleanupOrphanedMemory()) {
        std::cout << "  ✅ cleanupOrphanedMemory() 执行成功\n";
    } else {
        std::cout << "  ❌ cleanupOrphanedMemory() 执行失败\n";
    }
    
    // 测试5: 多节点场景
    std::cout << "\n[测试5] 多节点场景...\n";
    {
        SharedMemoryTransportV3 node1, node2, node3;
        
        node1.initialize("node1");
        node2.initialize("node2");
        node3.initialize("node3");
        
        sleep(1);  // 等待注册完成
        
        count = SharedMemoryTransportV3::getActiveNodeCount();
        std::cout << "  创建3个节点后活跃数: " << count << "\n";
        if (count == 3) {
            std::cout << "  ✅ 检测到3个活跃节点\n";
        }
        
        std::cout << "  所有节点即将析构...\n";
    }
    
    std::cout << "\n[测试6] 所有节点析构后检查...\n";
    sleep(1);
    count = SharedMemoryTransportV3::getActiveNodeCount();
    std::cout << "  活跃节点数: " << count << "\n";
    if (count == 0 || count == -1) {
        std::cout << "  ✅ 所有节点清理成功\n";
    } else {
        std::cout << "  ❌ 还有节点未清理\n";
    }
    
    std::cout << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║  测试完成!                           ║\n";
    std::cout << "╚════════════════════════════════════════╝\n";
    
    return 0;
}
