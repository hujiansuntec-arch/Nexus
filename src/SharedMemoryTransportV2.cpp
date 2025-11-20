// Lock-Free Shared Memory Transport v2 Implementation
#include "SharedMemoryTransportV2.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <chrono>
#include <thread>

namespace librpc {

SharedMemoryTransportV2::SharedMemoryTransportV2() 
    : initialized_(false)
    , my_node_index_(UINT32_MAX)
    , shm_ptr_(nullptr)
    , shm_size_(0)
    , shm_fd_(-1)
    , receiving_(false) {
}

SharedMemoryTransportV2::~SharedMemoryTransportV2() {
    stopReceiving();
    
    // Stop heartbeat thread
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    
    // Check if we're the last node before unregistering
    bool should_cleanup = false;
    if (shm_ptr_) {
        auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
        
        // Unregister this node
        unregisterNode();
        
        // Check if this was the last node
        uint32_t remaining_nodes = shm->control.num_nodes.load(std::memory_order_acquire);
        if (remaining_nodes == 0) {
            // We are the last node, schedule cleanup
            should_cleanup = true;
        }
    }
    
    // Unmap shared memory
    if (shm_ptr_) {
        munmap(shm_ptr_, shm_size_);
        shm_ptr_ = nullptr;
    }
    
    // Close fd
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
    
    // If we were the last node, unlink the shared memory
    if (should_cleanup) {
        shm_unlink(SHM_NAME);
        std::cout << "[SHM-V2] Last node cleaned up shared memory" << std::endl;
    }
}

bool SharedMemoryTransportV2::initialize(const std::string& node_id, const Config& config) {
    if (initialized_) {
        return false;
    }
    
    node_id_ = node_id;
    config_ = config;
    
    // Calculate shared memory size
    shm_size_ = sizeof(SharedMemoryRegion);
    
    std::cout << "[SHM-V2] Initializing with " << shm_size_ / 1024 / 1024 
              << "MB shared memory" << std::endl;
    
    // Open or create shared memory
    shm_fd_ = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd_ < 0) {
        std::cerr << "[SHM-V2] Failed to open shared memory" << std::endl;
        return false;
    }
    
    // Set size
    if (ftruncate(shm_fd_, shm_size_) < 0) {
        std::cerr << "[SHM-V2] Failed to set shared memory size" << std::endl;
        close(shm_fd_);
        return false;
    }
    
    // Map memory
    shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        std::cerr << "[SHM-V2] Failed to map shared memory" << std::endl;
        close(shm_fd_);
        return false;
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    
    // Initialize control block if needed (first node)
    uint32_t expected_magic = 0;
    if (shm->control.magic.compare_exchange_strong(expected_magic, MAGIC, 
                                                   std::memory_order_acq_rel)) {
        // We're the first node - initialize
        shm->control.version.store(VERSION, std::memory_order_release);
        shm->control.num_nodes.store(0, std::memory_order_release);
        std::cout << "[SHM-V2] Initialized shared memory (first node)" << std::endl;
    } else {
        // Verify version
        uint32_t version = shm->control.version.load(std::memory_order_acquire);
        if (version != VERSION) {
            std::cerr << "[SHM-V2] Version mismatch: expected " << VERSION 
                      << ", got " << version << std::endl;
            munmap(shm_ptr_, shm_size_);
            close(shm_fd_);
            shm_ptr_ = nullptr;
            shm_fd_ = -1;
            return false;
        }
    }
    
    // Register this node
    if (!registerNode()) {
        munmap(shm_ptr_, shm_size_);
        close(shm_fd_);
        shm_ptr_ = nullptr;
        shm_fd_ = -1;
        return false;
    }
    
    initialized_ = true;
    std::cout << "[SHM-V2] Node '" << node_id_ << "' initialized at index " 
              << my_node_index_ << std::endl;
    
    return true;
}

bool SharedMemoryTransportV2::registerNode() {
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    
    // Find empty slot (cleanup will be done by heartbeat thread)
    for (size_t i = 0; i < MAX_NODES; i++) {
        auto& node = shm->nodes[i];
        
        // Try to claim this slot
        uint32_t expected = 0;
        if (node.flags.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            // Successfully claimed
            strncpy(node.node_id, node_id_.c_str(), sizeof(node.node_id) - 1);
            node.node_id[sizeof(node.node_id) - 1] = '\0';
            node.pid = getpid();
            node.sender_index = static_cast<uint32_t>(i);
            node.last_heartbeat.store(getCurrentTimeMs(), std::memory_order_release);
            
            my_node_index_ = static_cast<uint32_t>(i);
            shm->control.num_nodes.fetch_add(1, std::memory_order_release);
            
            return true;
        }
    }
    
    std::cerr << "[SHM-V2] No free slots (max " << MAX_NODES << " nodes)" << std::endl;
    return false;
}

void SharedMemoryTransportV2::unregisterNode() {
    if (!initialized_ || my_node_index_ == UINT32_MAX) {
        return;
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    auto& node = shm->nodes[my_node_index_];
    
    // Mark as free
    node.flags.store(0, std::memory_order_release);
    shm->control.num_nodes.fetch_sub(1, std::memory_order_release);
    
    my_node_index_ = UINT32_MAX;
    initialized_ = false;
}

uint32_t SharedMemoryTransportV2::findNodeIndex(const std::string& node_id) {
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    
    for (size_t i = 0; i < MAX_NODES; i++) {
        auto& node = shm->nodes[i];
        if ((node.flags.load(std::memory_order_acquire) & 1) &&
            strcmp(node.node_id, node_id.c_str()) == 0) {
            return static_cast<uint32_t>(i);
        }
    }
    
    return UINT32_MAX;
}

bool SharedMemoryTransportV2::send(const std::string& dest_node_id, 
                                   const uint8_t* data, size_t size) {
    if (!initialized_) {
        return false;
    }
    
    uint32_t dest_index = findNodeIndex(dest_node_id);
    if (dest_index == UINT32_MAX) {
        return false;  // Destination not found
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    
    // Write to queue: receiver_queues[dest_index][my_sender_index]
    auto& queue = shm->receiver_queues[dest_index].queues[my_node_index_];
    
    bool success = queue.tryWrite(node_id_.c_str(), data, size);
    if (success) {
        stats_messages_sent_.fetch_add(1, std::memory_order_relaxed);
        stats_bytes_sent_.fetch_add(size, std::memory_order_relaxed);
    }
    
    return success;
}

bool SharedMemoryTransportV2::broadcast(const uint8_t* data, size_t size) {
    if (!initialized_) {
        return false;
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    bool sent_any = false;
    
    // Send to all other nodes
    for (size_t i = 0; i < MAX_NODES; i++) {
        if (i == my_node_index_) {
            continue;  // Skip self
        }
        
        auto& node = shm->nodes[i];
        if (!(node.flags.load(std::memory_order_acquire) & 1)) {
            continue;  // Node not active
        }
        
        // Write to this receiver's queue
        auto& queue = shm->receiver_queues[i].queues[my_node_index_];
        if (queue.tryWrite(node_id_.c_str(), data, size)) {
            sent_any = true;
            stats_messages_sent_.fetch_add(1, std::memory_order_relaxed);
            stats_bytes_sent_.fetch_add(size, std::memory_order_relaxed);
        }
    }
    
    return sent_any;
}

void SharedMemoryTransportV2::setReceiveCallback(ReceiveCallback callback) {
    receive_callback_ = callback;
}

void SharedMemoryTransportV2::startReceiving() {
    if (receiving_.load()) {
        return;  // Already receiving
    }
    
    receiving_.store(true);
    receive_thread_ = std::thread(&SharedMemoryTransportV2::receiveLoop, this);
    heartbeat_thread_ = std::thread(&SharedMemoryTransportV2::heartbeatLoop, this);
}

void SharedMemoryTransportV2::stopReceiving() {
    if (!receiving_.load()) {
        return;
    }
    
    receiving_.store(false);
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
}

void SharedMemoryTransportV2::receiveLoop() {
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    
    // My receive queues: receiver_queues[my_node_index][sender_index]
    auto& my_queues = shm->receiver_queues[my_node_index_];
    
    char from_node_id[64];
    uint8_t buffer[LockFreeRingBuffer<QUEUE_CAPACITY>::MAX_MSG_SIZE];
    size_t msg_size;
    
    while (receiving_.load()) {
        bool received_any = false;
        
        // Poll all sender queues (round-robin)
        for (size_t sender_idx = 0; sender_idx < MAX_NODES; sender_idx++) {
            auto& queue = my_queues.queues[sender_idx];
            
            // Try to read from this queue
            while (queue.tryRead(from_node_id, buffer, msg_size)) {
                received_any = true;
                
                stats_messages_received_.fetch_add(1, std::memory_order_relaxed);
                stats_bytes_received_.fetch_add(msg_size, std::memory_order_relaxed);
                
                // Deliver to callback
                if (receive_callback_) {
                    receive_callback_(buffer, msg_size, std::string(from_node_id));
                }
            }
        }
        
        if (!received_any) {
            // No messages - very brief sleep to avoid busy-waiting
            // 1μs is aggressive but ensures minimal latency
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

SharedMemoryTransportV2::TransportStats SharedMemoryTransportV2::getStats() const {
    TransportStats stats;
    stats.messages_sent = stats_messages_sent_.load(std::memory_order_relaxed);
    stats.messages_received = stats_messages_received_.load(std::memory_order_relaxed);
    stats.bytes_sent = stats_bytes_sent_.load(std::memory_order_relaxed);
    stats.bytes_received = stats_bytes_received_.load(std::memory_order_relaxed);
    stats.messages_dropped = 0;
    stats.avg_queue_depth = 0.0;
    
    if (!initialized_) {
        return stats;
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    
    // Calculate average queue depth and total drops
    size_t total_depth = 0;
    size_t active_queues = 0;
    
    for (size_t i = 0; i < MAX_NODES; i++) {
        auto& queue = shm->receiver_queues[my_node_index_].queues[i];
        auto queue_stats = queue.getStats();
        
        if (queue_stats.messages_written > 0) {
            active_queues++;
            total_depth += queue_stats.current_size;
            stats.messages_dropped += queue_stats.messages_dropped;
        }
    }
    
    if (active_queues > 0) {
        stats.avg_queue_depth = static_cast<double>(total_depth) / active_queues;
    }
    
    return stats;
}

std::vector<std::string> SharedMemoryTransportV2::getLocalNodes() const {
    std::vector<std::string> result;
    
    if (!initialized_) {
        return result;
    }
    
    auto* shm = static_cast<const SharedMemoryRegion*>(shm_ptr_);
    
    // Scan all node entries
    for (size_t i = 0; i < MAX_NODES; i++) {
        const auto& node = shm->nodes[i];
        uint32_t flags = node.flags.load(std::memory_order_acquire);
        
        if (flags & 1) {  // Node is active
            result.emplace_back(node.node_id);
        }
    }
    
    return result;
}

bool SharedMemoryTransportV2::isLocalNode(const std::string& node_id) const {
    if (!initialized_) {
        return false;
    }
    
    auto* shm = static_cast<const SharedMemoryRegion*>(shm_ptr_);
    
    // Scan all node entries
    for (size_t i = 0; i < MAX_NODES; i++) {
        const auto& node = shm->nodes[i];
        uint32_t flags = node.flags.load(std::memory_order_acquire);
        
        if ((flags & 1) && strcmp(node.node_id, node_id.c_str()) == 0) {
            return true;
        }
    }
    
    return false;
}

uint64_t SharedMemoryTransportV2::getCurrentTimeMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SharedMemoryTransportV2::heartbeatLoop() {
    if (!initialized_) {
        return;
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    
    // Immediately update heartbeat on start (before first cleanup)
    if (my_node_index_ < MAX_NODES) {
        auto& my_node = shm->nodes[my_node_index_];
        my_node.last_heartbeat.store(getCurrentTimeMs(), std::memory_order_release);
    }
    
    while (receiving_.load()) {
        // Update our heartbeat timestamp
        if (my_node_index_ < MAX_NODES) {
            auto& my_node = shm->nodes[my_node_index_];
            my_node.last_heartbeat.store(getCurrentTimeMs(), std::memory_order_release);
        }
        
        // Periodically cleanup stale nodes
        cleanupStaleNodes();
        
        // Sleep for heartbeat interval
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
    }
}

void SharedMemoryTransportV2::cleanupStaleNodes() {
    if (!initialized_) {
        return;
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(shm_ptr_);
    uint64_t now = getCurrentTimeMs();
    
    // Scan all nodes and cleanup stale ones
    for (size_t i = 0; i < MAX_NODES; i++) {
        auto& node = shm->nodes[i];
        uint32_t flags = node.flags.load(std::memory_order_acquire);
        
        if (flags & 1) {  // Node is marked as active
            uint64_t last_hb = node.last_heartbeat.load(std::memory_order_acquire);
            
            // Skip nodes with invalid heartbeat (0 = not initialized yet)
            if (last_hb == 0) {
                continue;
            }
            
            // Check if node has timed out (with safety check for wraparound)
            if (now > last_hb && (now - last_hb) > NODE_TIMEOUT_MS) {
                // Try to cleanup this stale node
                uint32_t expected = 1;
                if (node.flags.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                    std::cout << "[SHM-V2] Cleaned up stale node '" << node.node_id 
                              << "' at index " << i 
                              << " (last heartbeat " << (now - last_hb) << "ms ago, now=" << now << ", last_hb=" << last_hb << ")" << std::endl;
                    shm->control.num_nodes.fetch_sub(1, std::memory_order_release);
                }
            }
        }
    }
}

// Static utility methods for shared memory management
bool SharedMemoryTransportV2::cleanupOrphanedMemory() {
    // Try to unlink the shared memory
    int result = shm_unlink(SHM_NAME);
    
    if (result == 0) {
        std::cout << "[SHM-V2] Cleaned up orphaned shared memory" << std::endl;
        return true;
    } else if (errno == ENOENT) {
        // Shared memory doesn't exist, which is fine
        return true;
    } else {
        std::cerr << "[SHM-V2] Failed to cleanup shared memory: " << strerror(errno) << std::endl;
        return false;
    }
}

int SharedMemoryTransportV2::getActiveNodeCount() {
    // Try to open existing shared memory (read-only)
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd < 0) {
        if (errno == ENOENT) {
            // Shared memory doesn't exist
            return -1;
        }
        return -1;
    }
    
    // Map shared memory
    size_t shm_size = sizeof(SharedMemoryRegion);
    void* ptr = mmap(nullptr, shm_size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return -1;
    }
    
    auto* shm = static_cast<SharedMemoryRegion*>(ptr);
    
    // Check magic number
    uint32_t magic = shm->control.magic.load(std::memory_order_acquire);
    if (magic != MAGIC) {
        munmap(ptr, shm_size);
        close(fd);
        return -1;
    }
    
    // Get node count
    uint32_t count = shm->control.num_nodes.load(std::memory_order_acquire);
    
    munmap(ptr, shm_size);
    close(fd);
    
    return static_cast<int>(count);
}

} // namespace librpc
