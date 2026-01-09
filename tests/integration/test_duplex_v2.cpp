// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
// 改进的全双工通信性能测试 - 带流控和生命周期管理

#include "nexus/core/Node.h"
#include "nexus/utils/Logger.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <signal.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>

using namespace Nexus::rpc;

std::atomic<bool> g_running{true};
std::mutex g_cout_mutex;  // 保护输出，避免多线程交错

void signalHandler(int) {
    g_running = false;
}

class DuplexWorker {
public:
    // P2P Constructor
    DuplexWorker(const std::string& id, const std::string& peer_id, 
                 int duration, int msg_size, int target_rate)
        : node_id_(id)
        , peer_id_(peer_id)
        , duration_sec_(duration)
        , msg_size_(msg_size)
        , target_rate_(target_rate)
        , sent_(0)
        , received_(0)
        , echoed_(0)
        , is_shared_topic_(false) {
        
        my_topic_ = "to_" + node_id_;
        peer_topic_ = "to_" + peer_id_;
    }

    // Shared Topic Constructor
    DuplexWorker(const std::string& id, const std::string& shared_topic,
                 int duration, int msg_size, int target_rate, bool is_shared)
        : node_id_(id)
        , peer_id_("ALL")
        , duration_sec_(duration)
        , msg_size_(msg_size)
        , target_rate_(target_rate)
        , sent_(0)
        , received_(0)
        , echoed_(0)
        , is_shared_topic_(is_shared) {
        
        my_topic_ = shared_topic;
        peer_topic_ = shared_topic;
    }
    
    ~DuplexWorker() {
        if (!is_shared_topic_) {
             std::cout << "[" << node_id_ << "] 析构中...\n";
        }
        if (node_) {
            g_running = false;
            node_.reset();
        }
    }
    
    void run() {
        if (!is_shared_topic_ || node_id_.find("_0") != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            std::ostringstream oss;
            oss << "\n╔════════════════════════════════════════╗\n";
            oss << "║  [" << node_id_ << "] 全双工性能测试          ║\n";
            oss << "╠════════════════════════════════════════╣\n";
            oss << "║  模式: " << (is_shared_topic_ ? "共享Topic (One-to-All)" : "P2P (One-to-One)") << "\n";
            oss << "║  Topic: " << my_topic_ << "\n";
            oss << "║  消息大小: " << msg_size_ << " 字节\n";
            oss << "║  持续时间: " << duration_sec_ << " 秒\n";
            oss << "║  目标速率: " << target_rate_ << " msg/s\n";
            oss << "╚════════════════════════════════════════╝";
            std::cout << oss.str() << std::endl;  // 保持stdout用于脚本解析
        }
        
        node_ = createNode(node_id_);
        
        // Subscribe
        auto callback = [this](const std::string&, const std::string&,
                              const uint8_t* data, size_t size) {
            received_.fetch_add(1, std::memory_order_relaxed);
            
            // Debug: 打印前几个收到的消息来源，用于排查重复接收
            // 仅在低速模式下打印 (rate <= 10)
            if (target_rate_ <= 10) {
                 std::string msg((const char*)data, size);
                 // 消息内容就是 sender_node_id (前15字节)
                 NEXUS_DEBUG(node_id_) << "recv from " << msg;
            }
        };
        
        node_->subscribe("duplex", {my_topic_}, callback);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        std::vector<uint8_t> payload(msg_size_);
        std::memcpy(payload.data(), node_id_.c_str(),
                   std::min(node_id_.size(), size_t(15)));
        
        int send_interval_us = target_rate_ > 0 ? (1000000 / target_rate_) : 0;
        
        if (!is_shared_topic_ || node_id_.find("_0") != std::string::npos) {
            NEXUS_INFO(node_id_) << "开始发送...";
        }
        
        auto start = std::chrono::steady_clock::now();
        auto last_print = start;
        
        while (g_running) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - start).count();
            
            if (elapsed >= duration_sec_) {
                break;
            }
            
            node_->publish("duplex", peer_topic_,
                           std::string((char*)payload.data(), payload.size()));
            sent_.fetch_add(1, std::memory_order_relaxed);
            
            if (send_interval_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(send_interval_us));
            }
            
            if (!is_shared_topic_ || node_id_.find("_0") != std::string::npos) {
                auto since_print = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_print).count();
                if (since_print >= 1) {
                    printProgress(elapsed);
                    last_print = now;
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        if (!is_shared_topic_ || node_id_.find("_0") != std::string::npos) {
             printFinalStats(start);
        }
    }
    
    uint64_t getSent() const { return sent_.load(); }
    uint64_t getReceived() const { return received_.load(); }

private:
    void printProgress(int elapsed) {
        std::lock_guard<std::mutex> lock(g_cout_mutex);
        std::ostringstream oss;
        oss << "[" << node_id_ << "] " << elapsed << "s"
            << " | 发送:" << sent_.load()
            << " | 接收:" << received_.load();
        std::cout << oss.str() << "\r" << std::flush;  // 保持\r用于进度更新
    }
    
    void printFinalStats(std::chrono::steady_clock::time_point start) {
        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
        
        uint64_t sent = sent_.load();
        uint64_t received = received_.load();
        
        std::lock_guard<std::mutex> lock(g_cout_mutex);
        std::ostringstream oss;
        oss << "\n\n╔════════════════════════════════════════╗\n";
        oss << "║  [" << node_id_ << "] 最终统计 (" << ms << "ms)      ║\n";
        oss << "╠════════════════════════════════════════╣\n";
        oss << "║  发送消息: " << sent << "\n";
        oss << "║  接收消息: " << received << "\n";
        
        if (ms > 0) {
            double send_rate = (sent * 1000.0) / ms;
            double recv_rate = (received * 1000.0) / ms;
            oss << "║  发送速率: " << (int)send_rate << " msg/s\n";
            oss << "║  接收速率: " << (int)recv_rate << " msg/s\n";
        }
        oss << "╚════════════════════════════════════════╝";
        std::cout << oss.str() << std::endl;  // 保持stdout用于脚本解析
    }
    
private:
    std::string node_id_;
    std::string peer_id_;
    std::string my_topic_;
    std::string peer_topic_;
    int duration_sec_;
    int msg_size_;
    int target_rate_;
    bool is_shared_topic_;
    
    std::shared_ptr<Node> node_;
    std::atomic<uint64_t> sent_;
    std::atomic<uint64_t> received_;
    std::atomic<uint64_t> echoed_;
};

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // 多节点共享Topic模式
    // 用法: ./test_duplex_v2 multi <base_name> <count> <duration> <msg_size> <rate> [topic]
    if (argc >= 7 && std::string(argv[1]) == "multi") {
        std::string base_name = argv[2];
        int count = std::atoi(argv[3]);
        int duration = std::atoi(argv[4]);
        int msg_size = std::atoi(argv[5]);
        int rate = std::atoi(argv[6]);
        std::string safe_topic = (argc >= 8) ? argv[7] : "duplex_shared_channel";

        std::cout << "启动多节点共享Topic测试 (Topic: " << safe_topic << ")\n";
        std::cout << "节点数量: " << count << ", 进程内模拟\n" << std::flush;

        std::vector<std::unique_ptr<DuplexWorker>> workers;
        std::vector<std::thread> threads;

        for (int i = 0; i < count; ++i) {
            std::string id = base_name + "_" + std::to_string(i);
            // 所有节点都订阅和发送到同一个 safe_topic
            workers.push_back(std::make_unique<DuplexWorker>(
                id, safe_topic, duration, msg_size, rate, true
            ));
        }

        for (auto& worker : workers) {
            threads.emplace_back([&worker]() {
                worker->run();
            });
        }

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        uint64_t total_sent = 0;
        uint64_t total_recv = 0;
        for (const auto& w : workers) {
            total_sent += w->getSent();
            total_recv += w->getReceived();
        }

        std::cout << "\n\n=== 全部节点汇总 ===\n";
        std::cout << "总发送: " << total_sent << "\n";
        std::cout << "总接收: " << total_recv << "\n"; // Expect ~ sent * (count - 1)
        
        if (total_sent > 0 && count > 1) {
            double ratio = (double)total_recv / total_sent;
            double expected = (double)(count - 1);
            std::cout << "放大倍率: " << ratio << " (预期约: " << expected << ")\n";
        }

        std::cout << "[" << base_name << "] 程序正常退出\n" << std::flush;
        return 0;
    }

    if (argc < 6) {
        std::cout << "用法1 (P2P): " << argv[0] 
                  << " <node_id> <peer_id> <duration_sec> <msg_size> <rate_per_sec>\n";
        std::cout << "用法2 (Multi): " << argv[0] 
                  << " multi <base_name> <count> <duration_sec> <msg_size> <rate_per_sec> [topic]\n\n";
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
    
    std::cout << "\n[" << node_id << "] 程序正常退出\n" << std::flush;
    return 0;
}
