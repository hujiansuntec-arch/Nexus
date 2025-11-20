// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 测试自动清理功能

#include "SharedMemoryTransportV2.h"
#include <iostream>
#include <unistd.h>

using namespace librpc;

int main() {
    std::cout << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║  共享内存自动清理测试                ║\n";
    std::cout << "╚════════════════════════════════════════╝\n\n";
    
    // 测试1: getActiveNodeCount (应该返回-1或0)
    std::cout << "[测试1] 检查当前活跃节点数...\n";
    int count = SharedMemoryTransportV2::getActiveNodeCount();
    std::cout << "  活跃节点数: " << count << "\n";
    if (count == -1) {
        std::cout << "  ✅ 共享内存不存在\n";
    } else {
        std::cout << "  ⚠️  共享内存存在，有 " << count << " 个节点\n";
    }
    
    // 测试2: 创建节点后检查
    std::cout << "\n[测试2] 创建一个节点...\n";
    {
        SharedMemoryTransportV2 transport;
        if (!transport.initialize("test_node")) {
            std::cerr << "  ❌ 初始化失败\n";
            return 1;
        }
        std::cout << "  ✅ 节点创建成功\n";
        
        count = SharedMemoryTransportV2::getActiveNodeCount();
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
    count = SharedMemoryTransportV2::getActiveNodeCount();
    std::cout << "  活跃节点数: " << count << "\n";
    if (count == -1) {
        std::cout << "  ✅ 共享内存已自动清理\n";
    } else {
        std::cout << "  ❌ 共享内存未清理 (还有 " << count << " 个节点)\n";
    }
    
    // 测试4: 手动清理孤立内存
    std::cout << "\n[测试4] 测试手动清理功能...\n";
    if (SharedMemoryTransportV2::cleanupOrphanedMemory()) {
        std::cout << "  ✅ cleanupOrphanedMemory() 执行成功\n";
    } else {
        std::cout << "  ❌ cleanupOrphanedMemory() 执行失败\n";
    }
    
    // 测试5: 多节点场景
    std::cout << "\n[测试5] 多节点场景...\n";
    {
        SharedMemoryTransportV2 node1, node2, node3;
        
        node1.initialize("node1");
        node2.initialize("node2");
        node3.initialize("node3");
        
        count = SharedMemoryTransportV2::getActiveNodeCount();
        std::cout << "  创建3个节点后活跃数: " << count << "\n";
        if (count == 3) {
            std::cout << "  ✅ 检测到3个活跃节点\n";
        }
        
        // 只销毁node2
        std::cout << "  销毁node2...\n";
        node2.~SharedMemoryTransportV2();
        sleep(1);
        
        count = SharedMemoryTransportV2::getActiveNodeCount();
        std::cout << "  销毁1个节点后活跃数: " << count << "\n";
        if (count == 2) {
            std::cout << "  ✅ 正确减少到2个节点\n";
        }
        
        std::cout << "  其他节点即将析构...\n";
    }
    
    std::cout << "\n[测试6] 所有节点析构后检查...\n";
    sleep(1);
    count = SharedMemoryTransportV2::getActiveNodeCount();
    std::cout << "  活跃节点数: " << count << "\n";
    if (count == -1) {
        std::cout << "  ✅ 所有节点清理后共享内存已自动删除\n";
    } else {
        std::cout << "  ❌ 共享内存未完全清理\n";
    }
    
    std::cout << "\n╔════════════════════════════════════════╗\n";
    std::cout << "║  测试完成!                           ║\n";
    std::cout << "╚════════════════════════════════════════╝\n";
    
    return 0;
}
