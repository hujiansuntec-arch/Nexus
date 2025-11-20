// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 测试异常退出时的清理

#include "SharedMemoryTransportV2.h"
#include <iostream>
#include <unistd.h>
#include <signal.h>

using namespace librpc;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "用法: " << argv[0] << " <node_id>\n";
        std::cout << "示例:\n";
        std::cout << "  终端1: " << argv[0] << " node1\n";
        std::cout << "  终端2: " << argv[0] << " node2\n";
        std::cout << "  然后用 kill -9 杀死node1，观察node2是否能清理僵尸节点\n";
        return 1;
    }
    
    std::string node_id = argv[1];
    
    std::cout << "[" << node_id << "] 启动节点...\n";
    std::cout << "[" << node_id << "] PID: " << getpid() << "\n";
    
    SharedMemoryTransportV2 transport;
    if (!transport.initialize(node_id)) {
        std::cerr << "[" << node_id << "] 初始化失败\n";
        return 1;
    }
    
    std::cout << "[" << node_id << "] 节点初始化成功\n";
    
    // 开始轮询显示活跃节点
    while (true) {
        sleep(2);
        
        int count = SharedMemoryTransportV2::getActiveNodeCount();
        std::cout << "[" << node_id << "] 活跃节点数: " << count << "\n";
        
        auto nodes = transport.getLocalNodes();
        std::cout << "[" << node_id << "] 活跃节点列表: ";
        for (const auto& n : nodes) {
            std::cout << n << " ";
        }
        std::cout << "\n";
    }
    
    return 0;
}
