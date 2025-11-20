// Lock-Free Shared Memory Transport v2
// Uses SPSC queues for each sender-receiver pair
#pragma once

#include "LockFreeQueue.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>
#include <thread>

namespace librpc {

/**
 * @brief Lock-free shared memory transport using per-sender queues
 * 
 * Architecture:
 * - Each receiver has N queues (one per potential sender)
 * - Senders write to their dedicated queue (no lock needed)
 * - Receiver polls all its queues (round-robin or priority-based)
 * 
 * Benefits:
 * - Zero lock contention between senders
 * - Wait-free writes
 * - Predictable latency
 */
class SharedMemoryTransportV2 {
public:
    using ReceiveCallback = std::function<void(const uint8_t* data, size_t size,
                                              const std::string& from_node_id)>;

    static constexpr size_t MAX_NODES = 8;      // Maximum nodes (reduced from 32 to save memory)
    static constexpr size_t QUEUE_CAPACITY = 1024;  // Messages per queue (reduced from 8192)
    
    struct Config {
        size_t queue_capacity;
        bool enable_stats;
        
        Config() : queue_capacity(QUEUE_CAPACITY), enable_stats(true) {}
    };
    
    SharedMemoryTransportV2();
    ~SharedMemoryTransportV2();
    
    /**
     * @brief Initialize transport
     * @param node_id Unique node identifier
     * @param config Configuration
     * @return true if successful
     */
    bool initialize(const std::string& node_id, const Config& config = Config());
    
    /**
     * @brief Send data to specific node
     * @param dest_node_id Destination node ID
     * @param data Data buffer
     * @param size Data size
     * @return true if sent (may be queued)
     */
    bool send(const std::string& dest_node_id, const uint8_t* data, size_t size);
    
    /**
     * @brief Broadcast to all nodes
     * @param data Data buffer
     * @param size Data size
     * @return true if broadcasted to at least one node
     */
    bool broadcast(const uint8_t* data, size_t size);
    
    /**
     * @brief Set receive callback
     */
    void setReceiveCallback(ReceiveCallback callback);
    
    /**
     * @brief Start receiving thread
     */
    void startReceiving();
    
    /**
     * @brief Stop receiving thread
     */
    void stopReceiving();
    
    /**
     * @brief Get node ID
     */
    std::string getNodeId() const { return node_id_; }
    
    /**
     * @brief Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    /**
     * @brief Get all local node IDs (for compatibility with legacy API)
     * @return Vector of active node IDs in shared memory
     */
    std::vector<std::string> getLocalNodes() const;
    
    /**
     * @brief Check if a node is local (in shared memory)
     * @param node_id Node identifier to check
     * @return true if node is active in shared memory
     */
    bool isLocalNode(const std::string& node_id) const;
    
    /**
     * @brief Clean up orphaned shared memory (static utility)
     * Removes shared memory if no processes are using it
     * @return true if cleanup succeeded or nothing to clean
     */
    static bool cleanupOrphanedMemory();
    
    /**
     * @brief Get count of active nodes in shared memory (static utility)
     * @return Number of active nodes, or -1 if shared memory doesn't exist
     */
    static int getActiveNodeCount();
    
    /**
     * @brief Transport statistics
     */
    struct TransportStats {
        uint64_t messages_sent;
        uint64_t messages_received;
        uint64_t messages_dropped;
        uint64_t bytes_sent;
        uint64_t bytes_received;
        double avg_queue_depth;
    };
    
    TransportStats getStats() const;
    
private:
    // Shared memory structures
    
    struct NodeEntry {
        std::atomic<uint32_t> flags;  // Bit 0: valid
        char node_id[60];
        pid_t pid;
        uint32_t sender_index;  // This node's index as a sender
        std::atomic<uint64_t> last_heartbeat;  // Timestamp in milliseconds
    };
    
    struct alignas(64) ControlBlock {
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> version;
        std::atomic<uint32_t> num_nodes;
        char padding[52];
    };
    
    /**
     * @brief Per-receiver queue array
     * Each receiver has MAX_NODES queues (indexed by sender_index)
     */
    struct ReceiverQueues {
        LockFreeRingBuffer<QUEUE_CAPACITY> queues[MAX_NODES];
    };
    
    struct SharedMemoryRegion {
        ControlBlock control;
        NodeEntry nodes[MAX_NODES];
        ReceiverQueues receiver_queues[MAX_NODES];  // [receiver_idx][sender_idx]
    };
    
    static constexpr uint32_t MAGIC = 0x4C465348;  // "LFSH" = Lock-Free SHared memory
    static constexpr uint32_t VERSION = 3;
    static constexpr const char* SHM_NAME = "/librpc_shm_v2";
    
    // Helper methods
    bool registerNode();
    void unregisterNode();
    void receiveLoop();
    void heartbeatLoop();
    void cleanupStaleNodes();
    uint32_t findNodeIndex(const std::string& node_id);
    uint64_t getCurrentTimeMs() const;
    
    // State
    std::string node_id_;
    Config config_;
    bool initialized_;
    uint32_t my_node_index_;  // My index in nodes array
    
    // Shared memory
    void* shm_ptr_;
    size_t shm_size_;
    int shm_fd_;
    
    // Receive thread
    std::thread receive_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> receiving_;
    ReceiveCallback receive_callback_;
    
    // Configuration constants
    static constexpr uint64_t HEARTBEAT_INTERVAL_MS = 1000;  // 1 second
    static constexpr uint64_t NODE_TIMEOUT_MS = 5000;        // 5 seconds
    
    // Statistics
    std::atomic<uint64_t> stats_messages_sent_{0};
    std::atomic<uint64_t> stats_messages_received_{0};
    std::atomic<uint64_t> stats_bytes_sent_{0};
    std::atomic<uint64_t> stats_bytes_received_{0};
};

} // namespace librpc
