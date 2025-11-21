#include "SharedMemoryTransportV3.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace librpc {

SharedMemoryTransportV3::SharedMemoryTransportV3()
    : initialized_(false)
    , my_shm_ptr_(nullptr)
    , my_shm_fd_(-1)
    , my_shm_(nullptr)
    , receiving_(false)
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
    
    std::cout << "[SHM-V3] Node " << node_id_ << " destroyed" << std::endl;
}

bool SharedMemoryTransportV3::initialize(const std::string& node_id, const Config& config) {
    if (initialized_) {
        return true;
    }
    
    if (node_id.empty()) {
        std::cerr << "[SHM-V3] Invalid node ID" << std::endl;
        return false;
    }
    
    node_id_ = node_id;
    config_ = config;
    
    // Initialize registry
    if (!registry_.initialize()) {
        std::cerr << "[SHM-V3] Failed to initialize registry" << std::endl;
        return false;
    }
    
    // Generate unique shared memory name
    my_shm_name_ = generateShmName();
    
    // Create my shared memory
    if (!createMySharedMemory()) {
        std::cerr << "[SHM-V3] Failed to create shared memory" << std::endl;
        return false;
    }
    
    // Register in registry
    if (!registry_.registerNode(node_id_, my_shm_name_)) {
        std::cerr << "[SHM-V3] Failed to register node" << std::endl;
        destroyMySharedMemory();
        return false;
    }
    
    // 🔧 两阶段提交：设置ready标志
    my_shm_->header.ready.store(true, std::memory_order_release);
    
    initialized_ = true;
    
    std::cout << "[SHM-V3] Node " << node_id_ << " initialized"
              << "\n  Shared memory: " << my_shm_name_
              << "\n  Queue capacity: " << config_.queue_capacity
              << "\n  Max inbound queues: " << MAX_INBOUND_QUEUES
              << std::endl;
    
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
            } else {
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
    
    std::cout << "[SHM-V3] Started receiving threads for " << node_id_ << std::endl;
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
    
    std::cout << "[SHM-V3] Stopped receiving threads for " << node_id_ << std::endl;
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
    
    std::cout << "[SHM-V3] Warmed up " << connected << " connections" << std::endl;
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
    std::cout << "[SHM-V3] Cleaning up orphaned shared memory..." << std::endl;
    
    bool cleaned = false;
    
    // First, cleanup the registry (this will also tell us about nodes)
    if (SharedMemoryRegistry::cleanupOrphanedRegistry()) {
        cleaned = true;
    }
    
    // Try to cleanup individual node shared memories
    // This is a best-effort approach - we scan for common patterns
    for (int pid = 1; pid < 65536; ++pid) {
        for (int hash = 0; hash < 256; ++hash) {
            char shm_name[128];
            snprintf(shm_name, sizeof(shm_name), "/librpc_node_%d_%08x", pid, hash);
            
            // Try to unlink (will fail if doesn't exist or in use)
            if (shm_unlink(shm_name) == 0) {
                std::cout << "[SHM-V3] Cleaned orphaned memory: " << shm_name << std::endl;
                cleaned = true;
            }
        }
    }
    
    if (cleaned) {
        std::cout << "[SHM-V3] Cleanup complete" << std::endl;
    } else {
        std::cout << "[SHM-V3] No orphaned memory found" << std::endl;
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
            std::cerr << "[SHM-V3] Failed to create shared memory: " << strerror(errno) << std::endl;
            return false;
        }
    }
    
    // Set size
    size_t shm_size = sizeof(NodeSharedMemory);
    if (ftruncate(my_shm_fd_, shm_size) < 0) {
        std::cerr << "[SHM-V3] Failed to set size: " << strerror(errno) << std::endl;
        close(my_shm_fd_);
        my_shm_fd_ = -1;
        shm_unlink(my_shm_name_.c_str());
        return false;
    }
    
    // Map memory with MAP_NORESERVE for lazy physical memory allocation
    my_shm_ptr_ = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, 
                       MAP_SHARED | MAP_NORESERVE, my_shm_fd_, 0);
    if (my_shm_ptr_ == MAP_FAILED) {
        std::cerr << "[SHM-V3] Failed to map memory: " << strerror(errno) << std::endl;
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
    my_shm_->header.max_queues.store(MAX_INBOUND_QUEUES);
    my_shm_->header.last_heartbeat.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    my_shm_->header.ready.store(false);  // 🔧 初始为未就绪
    
    // Initialize all queues
    for (size_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        my_shm_->queues[i].flags.store(0);
        my_shm_->queues[i].sender_id[0] = '\0';
        // 🔧 初始化流控字段
        my_shm_->queues[i].congestion_level.store(0);
        my_shm_->queues[i].drop_count.store(0);
    }
    
    size_t mb = shm_size / (1024 * 1024);
    std::cout << "[SHM-V3] Created shared memory: " << my_shm_name_ 
              << " (" << mb << " MB)" << std::endl;
    
    return true;
}

void SharedMemoryTransportV3::destroyMySharedMemory() {
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
        std::cout << "[SHM-V3] Destroyed shared memory: " << my_shm_name_ << std::endl;
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
        std::cerr << "[SHM-V3] Node not found in registry: " << target_node_id << std::endl;
        return false;
    }
    
    // Create connection structure
    RemoteConnection conn;
    conn.node_id = target_node_id;
    conn.shm_name = target_info.shm_name;
    
    // Open target node's shared memory
    conn.shm_fd = shm_open(target_info.shm_name.c_str(), O_RDWR, 0666);
    if (conn.shm_fd < 0) {
        std::cerr << "[SHM-V3] Failed to open remote shm " << target_info.shm_name 
                  << ": " << strerror(errno) << std::endl;
        return false;
    }
    
    // Map remote memory with MAP_NORESERVE for lazy physical memory allocation
    conn.shm_ptr = mmap(nullptr, sizeof(NodeSharedMemory), PROT_READ | PROT_WRITE, 
                        MAP_SHARED | MAP_NORESERVE, conn.shm_fd, 0);
    if (conn.shm_ptr == MAP_FAILED) {
        std::cerr << "[SHM-V3] Failed to map remote shm: " << strerror(errno) << std::endl;
        close(conn.shm_fd);
        return false;
    }
    
    NodeSharedMemory* remote_shm = static_cast<NodeSharedMemory*>(conn.shm_ptr);
    
    // Verify magic
    if (remote_shm->header.magic.load() != MAGIC) {
        std::cerr << "[SHM-V3] Invalid magic in remote shm" << std::endl;
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }
    
    // 🔧 两阶段提交：验证节点是否完全初始化
    if (!remote_shm->header.ready.load(std::memory_order_acquire)) {
        std::cerr << "[SHM-V3] Remote node not ready yet: " << target_node_id << std::endl;
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }
    
    // Find or create queue for me in target node's memory
    conn.my_queue = findOrCreateQueue(remote_shm, node_id_);
    if (!conn.my_queue) {
        std::cerr << "[SHM-V3] Failed to create queue in remote node" << std::endl;
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }
    
    conn.connected = true;
    remote_connections_[target_node_id] = conn;
    
    std::cout << "[SHM-V3] Connected to node: " << target_node_id 
              << " (shm: " << target_info.shm_name << ")" << std::endl;
    
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
    
    std::cout << "[SHM-V3] Disconnected from node: " << target_node_id << std::endl;
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
    if (num_queues >= MAX_INBOUND_QUEUES) {
        std::cerr << "[SHM-V3] Remote node queue limit reached" << std::endl;
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
            
            remote_shm->header.num_queues.fetch_add(1);
            
            std::cout << "[SHM-V3] Created queue in remote node for sender: " << sender_id << std::endl;
            return &q;
        }
    }
    
    return nullptr;
}

void SharedMemoryTransportV3::receiveLoop() {
    std::cout << "[SHM-V3] Receive loop started for " << node_id_ << std::endl;
    
    static constexpr size_t MESSAGE_SIZE = 2048;
    uint8_t buffer[MESSAGE_SIZE];
    
    while (receiving_.load()) {
        if (!my_shm_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        bool received_any = false;
        
        // 🔧 性能优化：只遍历所有slot，而不是num_queues
        // 因为num_queues可能不准确（中间有空洞）
        // 但增加早期退出优化
        uint32_t active_found = 0;
        uint32_t max_active = my_shm_->header.num_queues.load();
        
        for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
            InboundQueue& q = my_shm_->queues[i];
            
            uint32_t flags = q.flags.load(std::memory_order_relaxed);
            if ((flags & 0x3) != 0x3) {  // Not valid or not active
                continue;
            }
            
            active_found++;
            
            char from_node[64];
            size_t msg_size = MESSAGE_SIZE;
            if (q.queue.tryRead(from_node, buffer, msg_size)) {
                received_any = true;
                stats_messages_received_++;
                stats_bytes_received_ += msg_size;
                
                if (receive_callback_) {
                    receive_callback_(buffer, msg_size, from_node);
                }
            }
            
            // 🔧 早期退出：如果已经检查了所有活跃队列
            if (active_found >= max_active) {
                break;
            }
        }
        
        if (!received_any) {
            // 🔧 更短的休眠时间以降低延迟
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    
    std::cout << "[SHM-V3] Receive loop stopped for " << node_id_ << std::endl;
}

void SharedMemoryTransportV3::heartbeatLoop() {
    std::cout << "[SHM-V3] Heartbeat loop started for " << node_id_ << std::endl;
    
    while (receiving_.load()) {
        // Update my heartbeat in registry
        registry_.updateHeartbeat(node_id_);
        
        // Update my heartbeat in my shared memory
        if (my_shm_) {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            my_shm_->header.last_heartbeat.store(ms.count());
        }
        
        // Clean up stale nodes from registry
        registry_.cleanupStaleNodes(NODE_TIMEOUT_MS);
        
        // Clean up stale inbound queues
        cleanupStaleQueues();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
    }
    
    std::cout << "[SHM-V3] Heartbeat loop stopped for " << node_id_ << std::endl;
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
            std::cout << "[SHM-V3] Recycling queue slot " << i 
                      << " from stale sender: " << sender_id << std::endl;
            
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
                std::cout << "[SHM-V3] Drained " << drained 
                          << " pending messages from slot " << i << std::endl;
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

} // namespace librpc
