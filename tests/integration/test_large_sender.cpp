// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
//
// 大数据通道发送端测试

#include "nexus/core/Node.h"
#include "nexus/transport/LargeDataChannel.h"
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
        node_ = Nexus::rpc::createNode(node_id);
        if (!node_) {
            throw std::runtime_error("Failed to create node: " + node_id);
        }
        
        std::cout << "Large data sender initialized: " << node_id << std::endl;
        std::cout << "Using Node::sendLargeData() API" << std::endl;
        std::cout << "Will send to: group=large_data, channel=" << channel_name << std::endl;
    }
    
    // 发送大数据（使用新的 sendLargeData API）
    bool sendData(const std::string& topic, const std::vector<uint8_t>& data) {
        // 使用 Node::sendLargeData() - 内部会自动处理通道创建和通知
        std::cout << "📤 Sending data: topic=" << topic << ", size=" << data.size() << " bytes" << std::endl;
        auto error = node_->sendLargeData("large_data", channel_name_, topic, 
                                         data.data(), data.size());
        if (error != Nexus::rpc::Node::NO_ERROR) {
            std::cerr << "Failed to send large data, error: " << static_cast<int>(error) << std::endl;
            return false;
        }
        
        std::cout << "✅ Data sent successfully" << std::endl;
        return true;
    }
    
    // 检查是否可以写入指定大小的数据（用于流量控制）
    bool canWrite(size_t size) const {
        auto channel = node_->getLargeDataChannel(channel_name_);
        if (!channel) {
            // 通道还未创建，可以写入（首次写入会自动创建）
            return true;
        }
        return channel->canWrite(size);
    }
    
    // 打印统计信息（从 Node 接口获取通道信息）
    void printStats() const {
        auto channels = node_->findLargeDataChannels("large_data");
        
        std::cout << "\n=== 发送统计 ===\n"
                  << "通道数量: " << channels.size() << "\n";
        
        for (const auto& ch : channels) {
            if (ch.channel_name == channel_name_) {
                std::cout << "通道名称: " << ch.channel_name << "\n"
                          << "主题: " << ch.topic << "\n"
                          << "节点: " << ch.node_id << "\n";
            }
        }
        
        std::cout << std::endl;
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
    std::shared_ptr<Nexus::rpc::Node> node_;
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
        
        // ⚠️ 等待服务发现完成 - 给接收端时间注册服务
        std::cout << "Waiting for service discovery (2 seconds)..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
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
        int flow_control_waits = 0;  // 流量控制等待次数
        
        for (int i = 0; i < count; i++) {
            // 修改部分数据以区分不同的包
            uint32_t seq_num = i;
            memcpy(&test_data[0], &seq_num, sizeof(seq_num));
            
            // 流量控制：发送前检查缓冲区空间
            size_t required_size = data_size + 128;  // 数据 + header
            int flow_control_retries = 0;
            const int MAX_FLOW_CONTROL_RETRIES = 100;  // 最大等待10秒 (100 × 100ms)
            
            while (!sender.canWrite(required_size) && flow_control_retries < MAX_FLOW_CONTROL_RETRIES) {
                // 缓冲区空间不足，等待接收端消费数据
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                flow_control_retries++;
                flow_control_waits++;
                
                if (flow_control_retries % 10 == 0) {
                    std::cout << "⏳ 流量控制等待中... (" << flow_control_retries << "/" 
                              << MAX_FLOW_CONTROL_RETRIES << ")" << std::endl;
                }
            }
            
            if (flow_control_retries >= MAX_FLOW_CONTROL_RETRIES) {
                std::cerr << "❌ 流量控制超时，接收端可能未运行或处理过慢: " << i << std::endl;
                break;  // 退出发送循环
            }
            
            // 发送数据（已确保有足够空间）
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
                    // 发送失败（理论上不应该发生，因为已检查空间）
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    retries++;
                    retry_count++;
                }
            }
            
            if (!sent) {
                std::cerr << "❌ 发送失败: " << i << std::endl;
            }
            
            // 短暂延迟，避免发送过快
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time).count();
        
        // 打印结果
        std::cout << "\n=== 发送完成 ===\n"
                  << "成功: " << success_count << "/" << count << "\n"
                  << "重试: " << retry_count << "\n"
                  << "流量控制等待: " << flow_control_waits << " 次\n"
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
