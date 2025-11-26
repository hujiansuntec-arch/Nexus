// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
//
// 大数据通道发送端测试

#include "Node.h"
#include "LargeDataChannel.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>
#include <cstring>

class LargeDataSender {
public:
    LargeDataSender(const std::string& node_id, const std::string& channel_name) 
        : node_id_(node_id), channel_name_(channel_name) {
        
        // 创建V3节点
        node_ = librpc::createNode(node_id);
        if (!node_) {
            throw std::runtime_error("Failed to create node: " + node_id);
        }
        
        // 通过Node接口获取大数据通道（自动使用64MB+MAP_NORESERVE配置）
        large_channel_ = node_->getLargeDataChannel(channel_name);
        if (!large_channel_) {
            throw std::runtime_error("Failed to get large data channel: " + channel_name);
        }
        
        std::cout << "Large data sender initialized: " << node_id << std::endl;
        std::cout << "Using integrated Node API for large data channel" << std::endl;
    }
    
    // 发送大数据
    bool sendData(const std::string& topic, const std::vector<uint8_t>& data) {
        // 检查空间
        size_t required = sizeof(librpc::LargeDataHeader) + data.size();
        if (!large_channel_->canWrite(required)) {
            std::cerr << "Buffer full, available: " 
                      << large_channel_->getAvailableSpace() 
                      << ", required: " << required << std::endl;
            return false;
        }
        
        // 写入大数据到专用通道
        int64_t seq = large_channel_->write(topic, data.data(), data.size());
        if (seq < 0) {
            std::cerr << "Failed to write data" << std::endl;
            return false;
        }
        
        // 通过V3消息队列发送通知（仅128字节）
        librpc::LargeDataNotification notif{};
        notif.sequence = seq;
        notif.size = static_cast<uint32_t>(data.size());
        strncpy(notif.channel_name, channel_name_.c_str(), sizeof(notif.channel_name) - 1);
        strncpy(notif.topic, topic.c_str(), sizeof(notif.topic) - 1);
        
        // 将通知转换为字符串发送（broadcast需要string payload）
        std::string notif_str(reinterpret_cast<char*>(&notif), sizeof(notif));
        
        if (node_->broadcast("large_data", "data_ready", notif_str) != librpc::Node::NO_ERROR) {
            std::cerr << "Failed to send notification" << std::endl;
            return false;
        }
        
        return true;
    }
    
    // 打印统计信息
    void printStats() const {
        auto stats = large_channel_->getStats();
        std::cout << "\n=== 发送统计 ===\n"
                  << "总写入次数: " << stats.total_writes << "\n"
                  << "总写入字节: " << formatBytes(stats.total_bytes_written) << "\n"
                  << "当前占用: " << formatBytes(stats.current_usage) << "\n"
                  << "缓冲区容量: " << formatBytes(stats.capacity) << "\n"
                  << "使用率: " << std::fixed << std::setprecision(2) 
                  << stats.usage_percent() << "%\n"
                  << std::endl;
    }
    
private:
    static std::string formatBytes(uint64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = bytes;
        
        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }
        
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
        return std::string(buf);
    }
    
    std::string node_id_;
    std::string channel_name_;
    std::shared_ptr<librpc::Node> node_;
    std::shared_ptr<librpc::LargeDataChannel> large_channel_;
};

int main(int argc, char* argv[]) {
    // 参数：发送次数 数据大小(KB)
    int count = (argc > 1) ? atoi(argv[1]) : 100;
    int size_kb = (argc > 2) ? atoi(argv[2]) : 1024;  // 默认1MB
    
    std::cout << "Large data sender test\n"
              << "Count: " << count << "\n"
              << "Size: " << size_kb << " KB\n"
              << std::endl;
    
    try {
        LargeDataSender sender("sender", "test_channel");
        
        // 准备测试数据
        size_t data_size = size_kb * 1024;
        std::vector<uint8_t> test_data(data_size);
        
        // 填充可验证的数据模式
        for (size_t i = 0; i < test_data.size(); i++) {
            test_data[i] = (i & 0xFF);
        }
        
        // 开始发送
        auto start_time = std::chrono::steady_clock::now();
        int success_count = 0;
        int retry_count = 0;
        
        for (int i = 0; i < count; i++) {
            // 修改部分数据以区分不同的包
            uint32_t seq_num = i;
            memcpy(&test_data[0], &seq_num, sizeof(seq_num));
            
            // 发送
            bool sent = false;
            int retries = 0;
            
            while (!sent && retries < 10) {
                if (sender.sendData("test/data", test_data)) {
                    sent = true;
                    success_count++;
                    
                    // 每10次打印一次进度
                    if ((i + 1) % 10 == 0) {
                        std::cout << "已发送: " << (i + 1) << "/" << count 
                                  << " (" << (success_count * 100 / (i + 1)) << "%)"
                                  << std::endl;
                    }
                } else {
                    // 缓冲区满，等待
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    retries++;
                    retry_count++;
                }
            }
            
            if (!sent) {
                std::cerr << "Failed to send after retries: " << i << std::endl;
            }
            
            // 高频发送：可以调整延迟（例如60fps = 16.67ms）
            // std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();
        
        // 打印结果
        std::cout << "\n=== 发送完成 ===\n"
                  << "成功: " << success_count << "/" << count << "\n"
                  << "重试: " << retry_count << "\n"
                  << "耗时: " << duration << " ms\n"
                  << "平均速度: " << (success_count * 1000 / duration) << " 次/秒\n"
                  << "吞吐量: " << (success_count * data_size / 1024.0 / 1024.0 / duration * 1000) 
                  << " MB/s\n"
                  << std::endl;
        
        sender.printStats();
        
        // 等待接收端处理
        std::cout << "等待接收端处理...(10秒)\n";
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
