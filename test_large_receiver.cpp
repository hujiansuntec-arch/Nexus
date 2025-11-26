// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
//
// 大数据通道接收端测试

#include "Node.h"
#include "LargeDataChannel.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <iomanip>
#include <csignal>
#include <cstring>

class LargeDataReceiver {
public:
    LargeDataReceiver(const std::string& node_id, const std::string& channel_name)
        : node_id_(node_id), 
          channel_name_(channel_name),
          received_count_(0),
          total_bytes_(0),
          crc_errors_(0),
          running_(true) {
        
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
        
        // 订阅大数据通知
        std::vector<std::string> topics = {"data_ready"};
        node_->subscribe("large_data", topics,
            [this](const std::string& group, const std::string& topic,
                   const uint8_t* data, size_t size) {
                this->onDataReady(group, topic, data, size);
            });
        
        std::cout << "Large data receiver initialized: " << node_id << std::endl;
        std::cout << "Using integrated Node API for large data channel" << std::endl;
        
        // 记录开始时间
        start_time_ = std::chrono::steady_clock::now();
    }
    
    void run() {
        std::cout << "Receiver running, waiting for data...\n" << std::endl;
        
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    void stop() {
        running_ = false;
    }
    
    void printStats() const {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time_).count();
        
        auto channel_stats = large_channel_->getStats();
        
        std::cout << "\n=== 接收统计 ===\n"
                  << "接收次数: " << received_count_ << "\n"
                  << "接收字节: " << formatBytes(total_bytes_) << "\n"
                  << "CRC错误: " << crc_errors_ << "\n"
                  << "耗时: " << duration << " ms\n";
        
        if (duration > 0) {
            std::cout << "平均速度: " << (received_count_ * 1000 / duration) << " 次/秒\n"
                      << "吞吐量: " << (total_bytes_ / 1024.0 / 1024.0 / duration * 1000) 
                      << " MB/s\n";
        }
        
        std::cout << "\n=== 通道统计 ===\n"
                  << "总读取次数: " << channel_stats.total_reads << "\n"
                  << "总读取字节: " << formatBytes(channel_stats.total_bytes_read) << "\n"
                  << "当前占用: " << formatBytes(channel_stats.current_usage) << "\n"
                  << "使用率: " << std::fixed << std::setprecision(2)
                  << channel_stats.usage_percent() << "%\n"
                  << std::endl;
    }
    
private:
    void onDataReady(const std::string& group, const std::string& topic,
                     const uint8_t* data, size_t size) {
        // 解析通知
        if (size != sizeof(librpc::LargeDataNotification)) {
            std::cerr << "Invalid notification size: " << size << std::endl;
            return;
        }
        
        auto* notif = reinterpret_cast<const librpc::LargeDataNotification*>(data);
        
        // 从大数据通道读取
        librpc::LargeDataChannel::DataBlock block;
        if (!large_channel_->tryRead(block)) {
            std::cerr << "Failed to read data block, seq: " << notif->sequence << std::endl;
            return;
        }
        
        // 验证序列号
        if (block.header->sequence != notif->sequence) {
            std::cerr << "Sequence mismatch: block=" << block.header->sequence 
                      << ", notif=" << notif->sequence << std::endl;
        }
        
        // 验证数据大小
        if (block.size != notif->size) {
            std::cerr << "Size mismatch: block=" << block.size 
                      << ", notif=" << notif->size << std::endl;
        }
        
        // 处理数据（验证数据模式）
        if (!verifyData(block.data, block.size)) {
            crc_errors_++;
            std::cerr << "Data verification failed for seq: " 
                      << block.header->sequence << std::endl;
        }
        
        // 更新统计
        received_count_++;
        total_bytes_ += block.size;
        
        // 每10次打印一次
        if (received_count_ % 10 == 0) {
            std::cout << "已接收: " << received_count_ 
                      << ", 总字节: " << formatBytes(total_bytes_)
                      << ", Topic: " << block.getTopic()
                      << std::endl;
        }
        
        // 释放数据块（重要！）
        large_channel_->releaseBlock(block);
    }
    
    bool verifyData(const uint8_t* data, size_t size) const {
        // 验证数据模式（与发送端一致）
        if (size < 4) {
            return false;
        }
        
        // 读取序列号
        uint32_t seq_num;
        memcpy(&seq_num, data, sizeof(seq_num));
        
        // 验证后续数据
        for (size_t i = 4; i < std::min(size, size_t(1024)); i++) {
            if (data[i] != (i & 0xFF)) {
                return false;
            }
        }
        
        return true;
    }
    
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
    
    std::atomic<int> received_count_;
    std::atomic<uint64_t> total_bytes_;
    std::atomic<int> crc_errors_;
    std::atomic<bool> running_;
    
    std::chrono::steady_clock::time_point start_time_;
};

// 全局接收器指针（用于信号处理）
LargeDataReceiver* g_receiver = nullptr;

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ", stopping..." << std::endl;
    if (g_receiver) {
        g_receiver->stop();
    }
}

int main() {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    try {
        LargeDataReceiver receiver("receiver", "test_channel");
        g_receiver = &receiver;
        
        // 运行接收循环
        receiver.run();
        
        // 打印统计
        receiver.printStats();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
