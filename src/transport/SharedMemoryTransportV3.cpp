#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/core/NodeImpl.h"  // For handleNodeEvent callback
#include "nexus/utils/Logger.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>  // 🔧 epoll支持（FIFO模式）
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>   // 🔧 POSIX线程支持（Condition Variable）
#include <cstring>
#include <cerrno>     // For errno
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <dirent.h>
#include <signal.h>   // For kill() process detection

// QNX specific includes
#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <sys/procfs.h>
#endif

namespace Nexus {
namespace rpc {

SharedMemoryTransportV3::SharedMemoryTransportV3()
    : initialized_(false)
    , node_impl_(nullptr)  // Initialize before other members
    , my_shm_ptr_(nullptr)
    , my_shm_fd_(-1)
    , my_shm_(nullptr)
    , receiving_(false)
    , notify_mechanism_(NotifyMechanism::CONDITION_VARIABLE)
{
}

SharedMemoryTransportV3::~SharedMemoryTransportV3() {
    stopReceiving();
    
    // Disconnect from all remote nodes
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& pair : remote_connections_) {
            if (pair.second.shm_ptr && pair.second.shm_ptr != MAP_FAILED) {
                munmap(pair.second.shm_ptr, sizeof(NodeSharedMemory));
            }
            if (pair.second.shm_fd >= 0) {
                close(pair.second.shm_fd);
            }
        }
        remote_connections_.clear();
    }
    
    // Unregister from registry
    if (initialized_) {
        registry_.unregisterNode(node_id_);
    }
    
    // Destroy my shared memory
    destroyMySharedMemory();
    
    NEXUS_DEBUG("SHM-V3") << "Node " << node_id_ << " destroyed";
}

bool SharedMemoryTransportV3::initialize(const std::string& node_id, const Config& config) {
    if (initialized_) {
        return true;
    }
    
    if (node_id.empty()) {
        NEXUS_ERROR("SHM-V3") << "Invalid node ID";
        return false;
    }
    
    // 验证配置
    if (config.max_inbound_queues > MAX_INBOUND_QUEUES) {
        NEXUS_ERROR("SHM-V3") << "Invalid config: max_inbound_queues (" 
                  << config.max_inbound_queues << ") exceeds limit (" 
                  << MAX_INBOUND_QUEUES << ")";
        return false;
    }
    
    node_id_ = node_id;
    config_ = config;
    notify_mechanism_ = config.notify_mechanism;  // 保存通知机制配置
    
    // Initialize registry
    if (!registry_.initialize()) {
        NEXUS_ERROR("SHM-V3") << "Failed to initialize registry";
        return false;
    }
    
    // Generate unique shared memory name
    my_shm_name_ = generateShmName();
    
    // Create my shared memory
    if (!createMySharedMemory()) {
        NEXUS_ERROR("SHM-V3") << "Failed to create shared memory";
        return false;
    }
    
    // Register in registry
    if (!registry_.registerNode(node_id_, my_shm_name_)) {
        NEXUS_ERROR("SHM-V3") << "Failed to register node";
        destroyMySharedMemory();
        return false;
    }
    
    // 🔧 两阶段提交：设置ready标志
    my_shm_->header.ready.store(true, std::memory_order_release);
    
    initialized_ = true;
    
    const char* mechanism_name = "Unknown";
    if (notify_mechanism_ == NotifyMechanism::CONDITION_VARIABLE) {
        mechanism_name = "Condition Variable";
    } else if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
        mechanism_name = "POSIX Semaphore";
    } else if (notify_mechanism_ == NotifyMechanism::SMART_POLLING) {
        mechanism_name = "Smart Polling";
    }
    
    NEXUS_DEBUG("SHM-V3") << "Node " << node_id_ << " initialized"
              << "\n  Notify mechanism: " << mechanism_name
              << "\n  Shared memory: " << my_shm_name_
              << "\n  Queue capacity: " << config_.queue_capacity
              << "\n  Max inbound queues: " << MAX_INBOUND_QUEUES
             ;
    
    return true;
}

bool SharedMemoryTransportV3::send(const std::string& dest_node_id, const uint8_t* data, size_t size) {
    if (!initialized_) {
        return false;
    }
    
    if (dest_node_id == node_id_) {
        return false;  // Don't send to self
    }
    
    // Fast path: check if already connected
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = remote_connections_.find(dest_node_id);
        if (it != remote_connections_.end() && it->second.connected && it->second.my_queue) {
            InboundQueue* queue = it->second.my_queue;
            
            // 🔧 流控：检查拥塞等级
            uint32_t congestion = queue->congestion_level.load(std::memory_order_relaxed);
            if (congestion > 0 && congestion <= 100) {
                // 根据拥塞等级进行退避 (0-100 -> 0-1000μs)
                int backoff_us = static_cast<int>(congestion) * 10;
                if (backoff_us > 0 && backoff_us <= 1000) {
                    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                }
            }
            
            // 尝试发送
            bool success = queue->queue.tryWrite(node_id_.c_str(), data, size);
            if (success) {
                stats_messages_sent_++;
                stats_bytes_sent_ += size;
                
                // 🔧 通知接收端：根据通知机制选择
                if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
                    // 🔧 Semaphore模式：批量通知优化
                    // 只在pending_msgs从0变1时才sem_post，避免过度通知
                    uint32_t prev = queue->pending_msgs.fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        sem_post(&queue->notify_sem);  // 只有第一条消息触发通知
                    }
                } else {
                    // 🔧 Condition Variable模式：批量通知优化
                    // 只在pending_msgs从0变1时才signal，避免过度唤醒
                    uint32_t prev = queue->pending_msgs.fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        pthread_cond_signal(&queue->notify_cond);  // 只有第一条消息触发通知
                    }
                    // 注意：不需要持有mutex来signal（POSIX允许）
                }
                
                // 🔧 流控：成功发送，降低拥塞等级
                if (congestion > 0) {
                    queue->congestion_level.fetch_sub(1, std::memory_order_relaxed);
                }
            } else {
                stats_messages_dropped_++;
                
                // 🔧 流控：发送失败，提高拥塞等级和丢包计数
                queue->drop_count.fetch_add(1, std::memory_order_relaxed);
                if (congestion < 100) {
                    queue->congestion_level.fetch_add(5, std::memory_order_relaxed);  // 快速上升
                }
            }
            return success;
        }
    }
    
    // Slow path: establish connection first
    if (!connectToNode(dest_node_id)) {
        stats_messages_dropped_++;
        return false;
    }
    
    // Retry send after connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = remote_connections_.find(dest_node_id);
        if (it != remote_connections_.end() && it->second.connected && it->second.my_queue) {
            InboundQueue* queue = it->second.my_queue;
            
            bool success = queue->queue.tryWrite(node_id_.c_str(), data, size);
            if (success) {
                stats_messages_sent_++;
                stats_bytes_sent_ += size;
                
                // 🔧 通知接收端：根据通知机制选择
                if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
                    // 🔧 Semaphore模式：批量通知优化
                    uint32_t prev = queue->pending_msgs.fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        sem_post(&queue->notify_sem);
                    }
                } else {
                    // 🔧 Condition Variable模式：批量通知优化
                    uint32_t prev = queue->pending_msgs.fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        pthread_cond_signal(&queue->notify_cond);
                    }
                }
            } else{
                stats_messages_dropped_++;
                queue->drop_count.fetch_add(1, std::memory_order_relaxed);
                queue->congestion_level.store(10, std::memory_order_relaxed);  // 初始拥塞
            }
            return success;
        }
    }
    
    stats_messages_dropped_++;
    return false;
}

int SharedMemoryTransportV3::broadcast(const uint8_t* data, size_t size) {
    if (!initialized_) {
        return 0;
    }
    
    // Get all nodes from registry
    std::vector<NodeInfo> nodes = registry_.getAllNodes();
    int sent_count = 0;
    
    for (const auto& node : nodes) {
        if (node.node_id == node_id_) {
            continue;  // Skip self
        }
        
        if (node.active && send(node.node_id, data, size)) {
            sent_count++;
        }
    }
    
    return sent_count;
}

void SharedMemoryTransportV3::setReceiveCallback(ReceiveCallback callback) {
    receive_callback_ = callback;
}

void SharedMemoryTransportV3::startReceiving() {
    if (receiving_.load()) {
        return;
    }
    
    receiving_.store(true);
    
    // Start receive thread
    receive_thread_ = std::thread([this]() {
        receiveLoop();
    });
    
    // Start heartbeat thread
    heartbeat_thread_ = std::thread([this]() {
        heartbeatLoop();
    });
    
    NEXUS_DEBUG("SHM-V3") << "Started receiving threads for " << node_id_;
}

void SharedMemoryTransportV3::stopReceiving() {
    if (!receiving_.load()) {
        return;
    }
    
    receiving_.store(false);
    
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    
    NEXUS_DEBUG("SHM-V3") << "Stopped receiving threads for " << node_id_;
}

std::vector<std::string> SharedMemoryTransportV3::getLocalNodes() const {
    std::vector<std::string> node_ids;
    
    if (!initialized_) {
        return node_ids;
    }
    
    std::vector<NodeInfo> nodes = registry_.getAllNodes();
    for (const auto& node : nodes) {
        if (node.active) {
            node_ids.push_back(node.node_id);
        }
    }
    
    return node_ids;
}

bool SharedMemoryTransportV3::isLocalNode(const std::string& node_id) const {
    if (!initialized_) {
        return false;
    }
    
    return registry_.nodeExists(node_id);
}

int SharedMemoryTransportV3::getConnectionCount() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    int count = 0;
    for (const auto& pair : remote_connections_) {
        if (pair.second.connected) {
            count++;
        }
    }
    return count;
}

void SharedMemoryTransportV3::warmupConnections() {
    if (!initialized_) {
        return;
    }
    
    std::vector<NodeInfo> nodes = registry_.getAllNodes();
    int connected = 0;
    
    for (const auto& node : nodes) {
        if (node.node_id != node_id_ && node.active) {
            if (connectToNode(node.node_id)) {
                connected++;
            }
        }
    }
    
    NEXUS_DEBUG("SHM-V3") << "Warmed up " << connected << " connections";
}

SharedMemoryTransportV3::TransportStats SharedMemoryTransportV3::getStats() const {
    TransportStats stats;
    stats.messages_sent = stats_messages_sent_.load();
    stats.messages_received = stats_messages_received_.load();
    stats.messages_dropped = stats_messages_dropped_.load();
    stats.bytes_sent = stats_bytes_sent_.load();
    stats.bytes_received = stats_bytes_received_.load();
    stats.active_connections = getConnectionCount();
    stats.inbound_queues = my_shm_ ? my_shm_->header.num_queues.load() : 0;
    
    // Calculate average queue depth
    stats.avg_queue_depth = 0.0;
    if (my_shm_) {
        uint32_t num_queues = my_shm_->header.num_queues.load();
        if (num_queues > 0) {
            uint64_t total_depth = 0;
            int active_queues = 0;
            for (uint32_t i = 0; i < num_queues && i < MAX_INBOUND_QUEUES; ++i) {
                InboundQueue& q = my_shm_->queues[i];
                if ((q.flags.load() & 0x3) == 0x3) {
                    total_depth += q.queue.size();
                    active_queues++;
                }
            }
            if (active_queues > 0) {
                stats.avg_queue_depth = static_cast<double>(total_depth) / active_queues;
            }
        }
    }
    
    return stats;
}

bool SharedMemoryTransportV3::cleanupOrphanedMemory() {
    NEXUS_DEBUG("SHM-V3") << "Cleaning up orphaned shared memory...";
    
    size_t cleaned_count = 0;
    size_t total_freed = 0;
    
    // 辅助函数：检查进程是否存活
    auto isProcessAlive = [](int32_t pid) -> bool {
        if (pid <= 0) {
            return false;  // 无效PID
        }
        
        // kill(pid, 0) 不发送信号，只检查进程是否存在
        if (kill(pid, 0) == 0) {
            return true;  // 进程存在且有权限访问
        }
        
        // ESRCH: 进程不存在
        // EPERM: 进程存在但无权限（也算存活）
        return errno != ESRCH;
    };
    
    // 扫描共享内存目录，查找librpc_node_*文件
    // QNX uses /dev/shmem instead of /dev/shm
    #ifdef __QNXNTO__
    const char* shm_dir = "/dev/shmem";
    #else
    const char* shm_dir = "/dev/shm";
    #endif
    
    DIR* dir = opendir(shm_dir);
    if (!dir) {
        NEXUS_ERROR("SHM-V3") << "Failed to open " << shm_dir << ": " << strerror(errno);
        return false;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        
        // 只处理librpc_node_*的共享内存
        if (name.find("librpc_node_") != 0) {
            continue;
        }
        
        // 尝试打开共享内存
        std::string full_name = "/" + name;
        int fd = shm_open(full_name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            continue;
        }
        
        // 获取文件大小
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            continue;
        }
        
        size_t shm_size = st.st_size;
        
        // 映射共享内存以检查header
        void* addr = mmap(nullptr, sizeof(NodeHeader), PROT_READ, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            continue;
        }
        
        bool should_cleanup = false;
        std::string cleanup_reason;
        
        // 检查是否为有效的V3节点共享内存
        NodeHeader* header = static_cast<NodeHeader*>(addr);
        
        if (header->magic.load(std::memory_order_relaxed) == MAGIC) {
            // 检查PID是否存活
            int32_t owner_pid = header->owner_pid.load(std::memory_order_relaxed);
            
            if (owner_pid > 0 && !isProcessAlive(owner_pid)) {
                should_cleanup = true;
                cleanup_reason = "owner process dead (PID: " + std::to_string(owner_pid) + ")";
            }
        } else {
            // 不是有效的V3共享内存，可能是残留文件
            should_cleanup = true;
            cleanup_reason = "invalid magic number";
        }
        
        munmap(addr, sizeof(NodeHeader));
        close(fd);
        
        if (should_cleanup) {
            if (shm_unlink(full_name.c_str()) == 0) {
                cleaned_count++;
                total_freed += shm_size;
                NEXUS_DEBUG("SHM-V3") << "✓ Cleaned: " << name 
                          << " (" << (shm_size / 1024 / 1024) << " MB) - " 
                          << cleanup_reason;
            } else {
                NEXUS_ERROR("SHM-V3") << "✗ Failed to unlink " << name 
                          << ": " << strerror(errno);
            }
        }
    }
    
    closedir(dir);
    
    // 清理registry
    if (SharedMemoryRegistry::cleanupOrphanedRegistry()) {
        NEXUS_DEBUG("SHM-V3") << "✓ Cleaned orphaned registry";
    }
    
    if (cleaned_count > 0) {
        NEXUS_DEBUG("SHM-V3") << "Cleanup complete: removed " << cleaned_count 
                  << " node(s), freed " << (total_freed / 1024 / 1024) << " MB";
    } else {
        NEXUS_DEBUG("SHM-V3") << "No orphaned nodes found";
    }
    
    return true;
}

int SharedMemoryTransportV3::getActiveNodeCount() {
    // Create temporary registry to query
    SharedMemoryRegistry temp_registry;
    if (!temp_registry.initialize()) {
        return -1;  // Registry doesn't exist
    }
    
    return temp_registry.getActiveNodeCount();
}

// Private helper methods

bool SharedMemoryTransportV3::createMySharedMemory() {
    // Create shared memory
    my_shm_fd_ = shm_open(my_shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (my_shm_fd_ < 0) {
        // Maybe old instance exists, try to clean up
        shm_unlink(my_shm_name_.c_str());
        my_shm_fd_ = shm_open(my_shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        if (my_shm_fd_ < 0) {
            NEXUS_ERROR("SHM-V3") << "Failed to create shared memory: " << strerror(errno);
            return false;
        }
    }
    
    // Set size
    size_t shm_size = sizeof(NodeSharedMemory);
    if (ftruncate(my_shm_fd_, shm_size) < 0) {
        NEXUS_ERROR("SHM-V3") << "Failed to set size: " << strerror(errno);
        close(my_shm_fd_);
        my_shm_fd_ = -1;
        shm_unlink(my_shm_name_.c_str());
        return false;
    }
    
    // Map memory
    // Note: MAP_NORESERVE may not be supported on QNX, use MAP_SHARED only
    #ifdef __QNXNTO__
    my_shm_ptr_ = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, my_shm_fd_, 0);
    #else
    my_shm_ptr_ = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, 
                       MAP_SHARED | MAP_NORESERVE, my_shm_fd_, 0);
    #endif
    if (my_shm_ptr_ == MAP_FAILED) {
        NEXUS_ERROR("SHM-V3") << "Failed to map memory: " << strerror(errno);
        close(my_shm_fd_);
        my_shm_fd_ = -1;
        shm_unlink(my_shm_name_.c_str());
        return false;
    }
    
    my_shm_ = static_cast<NodeSharedMemory*>(my_shm_ptr_);
    
    // Initialize header
    my_shm_->header.magic.store(MAGIC);
    my_shm_->header.version.store(VERSION);
    my_shm_->header.num_queues.store(0);
    my_shm_->header.max_queues.store(config_.max_inbound_queues);  // 使用配置值
    my_shm_->header.last_heartbeat.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    my_shm_->header.ready.store(false);  // 🔧 初始为未就绪
    my_shm_->header.owner_pid.store(getpid(), std::memory_order_release);  // 🔧 记录进程PID
    
    // Initialize all queues
    for (size_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        my_shm_->queues[i].flags.store(0);
        my_shm_->queues[i].sender_id[0] = '\0';
        my_shm_->queues[i].pending_msgs.store(0);  // 🔧 待处理消息计数初始化
        // 🔧 初始化流控字段
        my_shm_->queues[i].congestion_level.store(0);
        my_shm_->queues[i].drop_count.store(0);
    }
    
    size_t mb = shm_size / (1024 * 1024);
    NEXUS_DEBUG("SHM-V3") << "Created shared memory: " << my_shm_name_ 
              << " (" << mb << " MB, max_queues=" << config_.max_inbound_queues << ")";
    
    return true;
}

void SharedMemoryTransportV3::destroyMySharedMemory() {
    // 🔧 注意：pthread对象是跨进程共享的，不能随意destroy
    // pthread_mutex/cond_destroy只应该在最后一个使用者退出时调用
    // 这里我们只是unmap，让操作系统在shm被删除时清理
    
    if (my_shm_ptr_ && my_shm_ptr_ != MAP_FAILED) {
        munmap(my_shm_ptr_, sizeof(NodeSharedMemory));
        my_shm_ptr_ = nullptr;
        my_shm_ = nullptr;
    }
    
    if (my_shm_fd_ >= 0) {
        close(my_shm_fd_);
        my_shm_fd_ = -1;
    }
    
    if (!my_shm_name_.empty()) {
        shm_unlink(my_shm_name_.c_str());
        NEXUS_DEBUG("SHM-V3") << "Destroyed shared memory: " << my_shm_name_;
    }
}

bool SharedMemoryTransportV3::connectToNode(const std::string& target_node_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    // Check if already connected
    auto it = remote_connections_.find(target_node_id);
    if (it != remote_connections_.end() && it->second.connected) {
        return true;
    }
    
    // Get target node info from registry
    NodeInfo target_info;
    if (!registry_.findNode(target_node_id, target_info)) {
        NEXUS_ERROR("SHM-V3") << "Node not found in registry: " << target_node_id;
        return false;
    }
    
    // Create connection structure
    RemoteConnection conn;
    conn.node_id = target_node_id;
    conn.shm_name = target_info.shm_name;
    
    // Open target node's shared memory
    conn.shm_fd = shm_open(target_info.shm_name.c_str(), O_RDWR, 0666);
    if (conn.shm_fd < 0) {
        NEXUS_ERROR("SHM-V3") << "Failed to open remote shm " << target_info.shm_name 
                  << ": " << strerror(errno);
        return false;
    }
    
    // Map remote memory
    // Note: MAP_NORESERVE may not be supported on QNX, use MAP_SHARED only
    #ifdef __QNXNTO__
    conn.shm_ptr = mmap(nullptr, sizeof(NodeSharedMemory), PROT_READ | PROT_WRITE, 
                        MAP_SHARED, conn.shm_fd, 0);
    #else
    conn.shm_ptr = mmap(nullptr, sizeof(NodeSharedMemory), PROT_READ | PROT_WRITE, 
                        MAP_SHARED | MAP_NORESERVE, conn.shm_fd, 0);
    #endif
    if (conn.shm_ptr == MAP_FAILED) {
        NEXUS_ERROR("SHM-V3") << "Failed to map remote shm: " << strerror(errno);
        close(conn.shm_fd);
        return false;
    }
    
    NodeSharedMemory* remote_shm = static_cast<NodeSharedMemory*>(conn.shm_ptr);
    
    // Verify magic
    if (remote_shm->header.magic.load() != MAGIC) {
        NEXUS_ERROR("SHM-V3") << "Invalid magic in remote shm";
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }
    
    // 🔧 两阶段提交：验证节点是否完全初始化
    if (!remote_shm->header.ready.load(std::memory_order_acquire)) {
        NEXUS_ERROR("SHM-V3") << "Remote node not ready yet: " << target_node_id;
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }
    
    // Find or create queue for me in target node's memory
    conn.my_queue = findOrCreateQueue(remote_shm, node_id_);
    if (!conn.my_queue) {
        NEXUS_ERROR("SHM-V3") << "Failed to create queue in remote node";
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }
    
    conn.connected = true;
    
    remote_connections_[target_node_id] = conn;
    
    NEXUS_DEBUG("SHM-V3") << "Connected to node: " << target_node_id 
              << " (shm: " << target_info.shm_name << ")";
    
    return true;
}

void SharedMemoryTransportV3::disconnectFromNode(const std::string& target_node_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    auto it = remote_connections_.find(target_node_id);
    if (it == remote_connections_.end()) {
        return;
    }
    
    RemoteConnection& conn = it->second;
    
    if (conn.shm_ptr && conn.shm_ptr != MAP_FAILED) {
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
    }
    
    if (conn.shm_fd >= 0) {
        close(conn.shm_fd);
    }
    
    remote_connections_.erase(it);
    
    NEXUS_DEBUG("SHM-V3") << "Disconnected from node: " << target_node_id;
}

SharedMemoryTransportV3::InboundQueue* SharedMemoryTransportV3::findOrCreateQueue(
    NodeSharedMemory* remote_shm, const std::string& sender_id) {
    // First, try to find existing queue
    uint32_t num_queues = remote_shm->header.num_queues.load();
    for (uint32_t i = 0; i < num_queues && i < MAX_INBOUND_QUEUES; ++i) {
        InboundQueue& q = remote_shm->queues[i];
        if ((q.flags.load() & 0x1) && strcmp(q.sender_id, sender_id.c_str()) == 0) {
            return &q;
        }
    }
    
    // Not found, create new queue
    uint32_t max_queues = remote_shm->header.max_queues.load();
    if (num_queues >= max_queues) {
        NEXUS_ERROR("SHM-V3") << "Remote node queue limit reached (" << max_queues << ")";
        return nullptr;
    }
    
    // Find free slot
    for (size_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        InboundQueue& q = remote_shm->queues[i];
        uint32_t expected = 0;
        if (q.flags.compare_exchange_strong(expected, 0x3)) {  // Try to claim: valid | active
            // Successfully claimed this slot
            strncpy(q.sender_id, sender_id.c_str(), sizeof(q.sender_id) - 1);
            q.sender_id[sizeof(q.sender_id) - 1] = '\0';
            
            // 🔧 根据通知机制初始化相应资源
            if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
                // 🔧 Semaphore模式：初始化进程间共享信号量
                if (sem_init(&q.notify_sem, 1, 0) != 0) {
                    NEXUS_ERROR("SHM-V3") << "sem_init failed: " << strerror(errno);
                    q.flags.store(0);  // 回滚
                    return nullptr;
                }
                NEXUS_DEBUG("SHM-V3") << "Created queue with Semaphore for sender: " << sender_id;
            } else {
                // Condition Variable模式：初始化pthread对象
                pthread_mutexattr_t mattr;
                pthread_mutexattr_init(&mattr);
                pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
                pthread_mutex_init(&q.notify_mutex, &mattr);
                pthread_mutexattr_destroy(&mattr);
                
                pthread_condattr_t cattr;
                pthread_condattr_init(&cattr);
                pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
                pthread_cond_init(&q.notify_cond, &cattr);
                pthread_condattr_destroy(&cattr);
                
                q.pending_msgs.store(0);
                
                NEXUS_DEBUG("SHM-V3") << "Created queue in remote node for sender: " << sender_id 
                          << " (using Condition Variable)";
            }
            
            remote_shm->header.num_queues.fetch_add(1);
            return &q;
        }
    }
    
    return nullptr;
}

void SharedMemoryTransportV3::receiveLoop() {
    if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
        receiveLoop_Semaphore();
    } else {
        receiveLoop_CV();
    }
}

// Condition Variable模式的接收循环（优化版）
void SharedMemoryTransportV3::receiveLoop_CV() {
    NEXUS_DEBUG("SHM-V3") << "Receive loop started for " << node_id_ << " (Condition Variable mode - optimized)";
    
    static constexpr size_t MESSAGE_SIZE = 2048;
    uint8_t buffer[MESSAGE_SIZE];
    
    // 🔧 缓存活跃队列列表，减少遍历开销
    std::vector<InboundQueue*> active_queues;
    uint32_t cached_num_queues = 0;
    int queue_refresh_counter = 0;
    const int QUEUE_REFRESH_INTERVAL = 100;
    
    // 🔧 自适应超时：根据消息流量动态调整
    int consecutive_empty_loops = 0;
    const int ADAPTIVE_THRESHOLD = 100;  // 100次空循环后切换到长超时
    
    while (receiving_.load()) {
        if (!my_shm_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 🔧 检测队列变化并更新缓存
        uint32_t current_num_queues = my_shm_->header.num_queues.load(std::memory_order_relaxed);
        queue_refresh_counter++;
        
        if (current_num_queues != cached_num_queues || 
            queue_refresh_counter >= QUEUE_REFRESH_INTERVAL || 
            active_queues.empty()) {
            
            active_queues.clear();
            for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
                InboundQueue& q = my_shm_->queues[i];
                uint32_t flags = q.flags.load(std::memory_order_relaxed);
                if ((flags & 0x3) == 0x3) {
                    active_queues.push_back(&q);
                }
            }
            cached_num_queues = current_num_queues;
            queue_refresh_counter = 0;
        }
        
        if (active_queues.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 🔧 步骤1：快速处理所有队列的消息（批量处理减少atomic操作）
        bool has_messages = false;
        
        for (auto* q : active_queues) {
            // 安全检查：验证队列仍然有效
            uint32_t flags = q->flags.load(std::memory_order_relaxed);
            if ((flags & 0x3) != 0x3) {
                continue;
            }
            
            // 🔧 批量处理该队列的所有消息
            int processed = 0;
            while (true) {
                char from_node[64];
                size_t msg_size = MESSAGE_SIZE;
                
                if (!q->queue.tryRead(from_node, buffer, msg_size)) {
                    break;  // 队列空了
                }
                
                stats_messages_received_++;
                stats_bytes_received_ += msg_size;
                processed++;
                has_messages = true;
                
                if (receive_callback_) {
                    NEXUS_DEBUG("SHM-V3") << "Received message from " << from_node 
                              << " (" << msg_size << " bytes)";
                    receive_callback_(buffer, msg_size, from_node);
                }
            }
            
            // 减少pending计数
            if (processed > 0) {
                q->pending_msgs.fetch_sub(processed, std::memory_order_release);
            }
        }
        
        // 🔧 步骤2：如果没有消息，使用condition variable等待
        if (!has_messages && !active_queues.empty()) {
            consecutive_empty_loops++;
            
            // 🔧 自适应超时：无消息时50ms降低CPU，有消息时5ms保持低延迟
            int timeout_ms = (consecutive_empty_loops > ADAPTIVE_THRESHOLD) ? 50 : 5;
            
            // 使用第一个活跃队列的CV等待
            InboundQueue* wait_queue = active_queues[0];
            
            pthread_mutex_lock(&wait_queue->notify_mutex);
            
            // 🔧 再次快速检查pending（双重检查避免信号丢失）
            if (wait_queue->pending_msgs.load(std::memory_order_acquire) == 0) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += timeout_ms * 1000000;  // ms转ns
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000;
                }
                
                pthread_cond_timedwait(&wait_queue->notify_cond, &wait_queue->notify_mutex, &ts);
            }
            
            pthread_mutex_unlock(&wait_queue->notify_mutex);
        } else {
            // 🔧 有消息时重置计数器，保持短超时以降低延迟
            consecutive_empty_loops = 0;
        }
    }
    
    NEXUS_DEBUG("SHM-V3") << "Receive loop stopped for " << node_id_ << " (CV mode)";
}

// 🔧 Semaphore模式的接收循环（优化版：真正利用sem_timedwait阻塞等待）
void SharedMemoryTransportV3::receiveLoop_Semaphore() {
    NEXUS_DEBUG("SHM-V3") << "Receive loop started for " << node_id_ << " (Semaphore mode - optimized)";
    
    static constexpr size_t MESSAGE_SIZE = 2048;
    uint8_t buffer[MESSAGE_SIZE];
    
    // 🔧 缓存活跃队列列表，定期更新以降低开销
    std::vector<InboundQueue*> active_queues;
    uint32_t cached_num_queues = 0;  // 缓存num_queues用于检测变化
    int queue_refresh_counter = 0;
    const int QUEUE_REFRESH_INTERVAL = 100;  // 每100次循环刷新一次队列列表
    
    // 🔧 自适应超时：有消息时使用短超时，无消息时使用长超时
    int consecutive_empty_loops = 0;
    const int ADAPTIVE_THRESHOLD = 50;  // 50次空循环后切换到长超时
    
    while (receiving_.load()) {
        if (!my_shm_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 🔧 检测队列变化：num_queues变化或定期刷新
        uint32_t current_num_queues = my_shm_->header.num_queues.load(std::memory_order_relaxed);
        queue_refresh_counter++;
        
        if (current_num_queues != cached_num_queues || 
            queue_refresh_counter >= QUEUE_REFRESH_INTERVAL || 
            active_queues.empty()) {
            
            active_queues.clear();
            for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
                InboundQueue& q = my_shm_->queues[i];
                uint32_t flags = q.flags.load(std::memory_order_relaxed);
                if ((flags & 0x3) == 0x3) {
                    active_queues.push_back(&q);
                }
            }
            cached_num_queues = current_num_queues;
            queue_refresh_counter = 0;
        }
        
        if (active_queues.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // 🔧 步骤1：快速轮询所有队列（无阻塞）
        bool has_messages = false;
        for (auto* q : active_queues) {
            // 🔧 安全检查：每次访问前验证队列仍然有效
            uint32_t flags = q->flags.load(std::memory_order_relaxed);
            if ((flags & 0x3) != 0x3) {
                continue;  // 队列已失效，跳过
            }
            
            int processed = 0;
            while (true) {
                char from_node[64];
                size_t msg_size = MESSAGE_SIZE;
                
                if (!q->queue.tryRead(from_node, buffer, msg_size)) {
                    break;  // 队列空了
                }
                
                stats_messages_received_++;
                stats_bytes_received_ += msg_size;
                has_messages = true;
                processed++;
                
                if (receive_callback_) {
                    receive_callback_(buffer, msg_size, from_node);
                }
            }
            
            // 🔧 处理完消息后，减少pending_msgs计数
            if (processed > 0) {
                q->pending_msgs.fetch_sub(processed, std::memory_order_release);
            }
        }
        
        // 🔧 步骤2：如果没有消息，使用sem_timedwait阻塞等待
        if (!has_messages && !active_queues.empty()) {
            consecutive_empty_loops++;
            
            // 🔧 自适应超时：空闲时50ms，繁忙时5ms
            int timeout_ms = (consecutive_empty_loops > ADAPTIVE_THRESHOLD) ? 50 : 5;
            
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            long timeout_ns = timeout_ms * 1000000L;
            timeout.tv_nsec += timeout_ns;
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec++;
                timeout.tv_nsec -= 1000000000;
            }
            
            // 🔧 等待任意队列的信号量（尝试所有队列直到成功或超时）
            bool got_signal = false;
            for (auto* q : active_queues) {
                if (sem_trywait(&q->notify_sem) == 0) {
                    got_signal = true;
                    break;
                }
            }
            
            // 如果没有立即可用的信号，等待第一个队列
            if (!got_signal) {
                sem_timedwait(&active_queues[0]->notify_sem, &timeout);
            }
        } else {
            // 有消息时重置空循环计数
            consecutive_empty_loops = 0;
        }
    }
    
    NEXUS_DEBUG("SHM-V3") << "Receive loop stopped for " << node_id_ << " (Semaphore mode)";
}

void SharedMemoryTransportV3::heartbeatLoop() {
    NEXUS_DEBUG("SHM-V3") << "Heartbeat loop started for " << node_id_;
    
    while (receiving_.load()) {
        // Update my heartbeat in registry
        registry_.updateHeartbeat(node_id_);
        
        // Update my heartbeat in my shared memory
        if (my_shm_) {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            my_shm_->header.last_heartbeat.store(ms.count());
        }
        
        // Get nodes before cleanup (for detecting removed nodes)
        std::vector<NodeInfo> nodes_before;
        if (node_impl_) {
            nodes_before = registry_.getAllNodes();
        }
        
        // Clean up stale nodes from registry
        int cleaned = registry_.cleanupStaleNodes(NODE_TIMEOUT_MS);
        
        // Notify NodeImpl about removed nodes (trigger NODE_LEFT events)
        if (cleaned > 0 && node_impl_) {
            auto nodes_after = registry_.getAllNodes();
            
            // Find which nodes were removed
            for (const auto& node : nodes_before) {
                // Skip self
                if (node.node_id == node_id_) {
                    continue;
                }
                
                // Check if node still exists
                bool found = false;
                for (const auto& n : nodes_after) {
                    if (n.node_id == node.node_id) {
                        found = true;
                        break;
                    }
                }
                
                // Node was removed - trigger NODE_LEFT event
                if (!found) {
                    NEXUS_DEBUG("SHM-V3") << "Heartbeat timeout detected for node: " 
                              << node.node_id << ", triggering NODE_LEFT event";
                    node_impl_->handleNodeEvent(node.node_id, false);
                }
            }
        }
        
        // Clean up stale inbound queues
        cleanupStaleQueues();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
    }
    
    NEXUS_DEBUG("SHM-V3") << "Heartbeat loop stopped for " << node_id_;
}

void SharedMemoryTransportV3::cleanupStaleQueues() {
    if (!my_shm_) {
        return;
    }
    
    // 🔧 FIX: 遍历所有slot，不只是num_queues
    // 因为slot可能在中间位置被释放
    for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        InboundQueue& q = my_shm_->queues[i];
        
        if ((q.flags.load() & 0x1) == 0) {
            continue;  // Already free
        }
        
        // Check if sender still exists in registry
        std::string sender_id = q.sender_id;
        if (!sender_id.empty() && !registry_.nodeExists(sender_id)) {
            NEXUS_DEBUG("SHM-V3") << "Recycling queue slot " << i 
                      << " from stale sender: " << sender_id;
            
            // 🔧 FIX: 彻底清空队列，避免内存泄漏
            char dummy_sender[64];
            uint8_t dummy_data[2048];
            size_t dummy_size = 2048;
            int drained = 0;
            while (q.queue.tryRead(dummy_sender, dummy_data, dummy_size)) {
                drained++;
                dummy_size = 2048;  // Reset for next read
            }
            if (drained > 0) {
                NEXUS_DEBUG("SHM-V3") << "Drained " << drained 
                          << " pending messages from slot " << i;
            }
            
            // 🔧 FIX: 完全重置slot，使其可被重用
            q.flags.store(0);  // Clear valid & active flags
            q.sender_id[0] = '\0';
            
            my_shm_->header.num_queues.fetch_sub(1);
        }
    }
}

std::string SharedMemoryTransportV3::generateShmName() {
    std::ostringstream oss;
    oss << "/librpc_node_" << getpid() << "_" << std::hex << std::setfill('0') 
        << std::setw(8) << (std::hash<std::string>{}(node_id_) & 0xFFFFFFFF);
    return oss.str();
}

} // namespace rpc
} // namespace Nexus
