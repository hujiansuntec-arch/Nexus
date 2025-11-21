// Dynamic Shared Memory Transport V3
// Each node has its own shared memory region for receiving
// Nodes connect to each other on-demand
#pragma once

#include "LockFreeQueue.h"
#include "SharedMemoryRegistry.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>

namespace librpc {

/**
 * @brief Dynamic lock-free shared memory transport
 * 
 * Architecture:
 * - Each node creates its own shared memory: /dev/shm/librpc_node_<unique_id>
 * - Node's memory contains inbound queues from all senders
 * - Queues are created on-demand when first message is sent
 * - No fixed node limit - scales dynamically
 * 
 * Benefits:
 * - No MAX_NODES limit (supports hundreds of nodes)
 * - Memory efficient (only allocate what's needed)
 * - Better isolation (each node manages its own memory)
 * - Same lock-free SPSC performance as V2
 */
class SharedMemoryTransportV3 {
public:
    using ReceiveCallback = std::function<void(const uint8_t* data, size_t size,
                                              const std::string& from_node_id)>;
    
    static constexpr size_t QUEUE_CAPACITY = 1024;
    static constexpr size_t MAX_INBOUND_QUEUES = 256;  // Max senders to this node
    
    struct Config {
        size_t queue_capacity;
        bool enable_stats;
        bool auto_cleanup;
        
        Config() 
            : queue_capacity(QUEUE_CAPACITY)
            , enable_stats(true)
            , auto_cleanup(true)
        {}
    };
    
    SharedMemoryTransportV3();
    ~SharedMemoryTransportV3();
    
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
     * @return true if sent
     */
    bool send(const std::string& dest_node_id, const uint8_t* data, size_t size);
    
    /**
     * @brief Broadcast to all nodes
     * @param data Data buffer
     * @param size Data size
     * @return Number of nodes broadcasted to
     */
    int broadcast(const uint8_t* data, size_t size);
    
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
     * @brief Get all local node IDs (nodes using shared memory)
     * @return Vector of active node IDs
     */
    std::vector<std::string> getLocalNodes() const;
    
    /**
     * @brief Check if a node is local (reachable via shared memory)
     * @param node_id Node identifier
     * @return true if node is active in registry
     */
    bool isLocalNode(const std::string& node_id) const;
    
    /**
     * @brief Get connection count
     * @return Number of connected remote nodes
     */
    int getConnectionCount() const;
    
    /**
     * @brief Warmup connections to all nodes (optional optimization)
     * Pre-establishes connections to avoid first-send latency
     */
    void warmupConnections();
    
    /**
     * @brief Clean up orphaned shared memory (static utility)
     * Removes registry and node shared memories if no processes are using them
     * @return true if cleanup succeeded or nothing to clean
     */
    static bool cleanupOrphanedMemory();
    
    /**
     * @brief Get count of active nodes in registry (static utility)
     * @return Number of active nodes, or -1 if registry doesn't exist
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
        int active_connections;
        int inbound_queues;
        double avg_queue_depth;  // Average depth of inbound queues
    };
    
    TransportStats getStats() const;
    
private:
    // Inbound queue (receiving from a sender)
    struct InboundQueue {
        char sender_id[64];
        std::atomic<uint32_t> flags;  // Bit 0: valid, Bit 1: active
        LockFreeRingBuffer<QUEUE_CAPACITY> queue;
        
        // 🔧 流控：背压机制
        std::atomic<uint32_t> congestion_level;  // 拥塞等级 0-100
        std::atomic<uint64_t> drop_count;        // 累计丢包数
        
        char padding[56];  // Cache line alignment (adjusted for new fields)
    };
    
    // Node's shared memory layout
    struct alignas(64) NodeHeader {
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> version;
        std::atomic<uint32_t> num_queues;
        std::atomic<uint32_t> max_queues;
        std::atomic<uint64_t> last_heartbeat;
        std::atomic<bool> ready;  // 🔧 两阶段提交：节点是否完全初始化
        char padding[35];
    };
    
    struct NodeSharedMemory {
        NodeHeader header;
        InboundQueue queues[MAX_INBOUND_QUEUES];
    };
    
    // Connection to a remote node
    struct RemoteConnection {
        std::string node_id;
        std::string shm_name;
        void* shm_ptr;
        int shm_fd;
        InboundQueue* my_queue;  // My queue in remote node's memory
        bool connected;
        
        RemoteConnection()
            : shm_ptr(nullptr)
            , shm_fd(-1)
            , my_queue(nullptr)
            , connected(false)
        {}
    };
    
    static constexpr uint32_t MAGIC = 0x4C524E33;  // "LRN3" = LibRpc Node v3
    static constexpr uint32_t VERSION = 1;
    
    // Helper methods
    bool createMySharedMemory();
    void destroyMySharedMemory();
    bool connectToNode(const std::string& target_node_id);
    void disconnectFromNode(const std::string& target_node_id);
    InboundQueue* findOrCreateQueue(NodeSharedMemory* remote_shm, const std::string& sender_id);
    void receiveLoop();
    void heartbeatLoop();
    void cleanupStaleQueues();
    std::string generateShmName();
    
    // State
    std::string node_id_;
    std::string my_shm_name_;
    Config config_;
    bool initialized_;
    
    // Registry
    SharedMemoryRegistry registry_;
    
    // My shared memory (for receiving)
    void* my_shm_ptr_;
    int my_shm_fd_;
    NodeSharedMemory* my_shm_;
    
    // Connections to remote nodes (for sending)
    std::map<std::string, RemoteConnection> remote_connections_;
    mutable std::mutex connections_mutex_;
    
    // Receive thread
    std::thread receive_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> receiving_;
    ReceiveCallback receive_callback_;
    
    // Configuration constants
    static constexpr uint64_t HEARTBEAT_INTERVAL_MS = 1000;  // 1 second
    static constexpr uint64_t NODE_TIMEOUT_MS = 5000;        // 5 seconds
    static constexpr uint64_t QUEUE_TIMEOUT_MS = 10000;      // 10 seconds
    
    // Statistics
    std::atomic<uint64_t> stats_messages_sent_{0};
    std::atomic<uint64_t> stats_messages_received_{0};
    std::atomic<uint64_t> stats_messages_dropped_{0};
    std::atomic<uint64_t> stats_bytes_sent_{0};
    std::atomic<uint64_t> stats_bytes_received_{0};
};

} // namespace librpc
