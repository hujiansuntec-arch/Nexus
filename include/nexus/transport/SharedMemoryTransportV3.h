// Dynamic Shared Memory Transport V3
// Each node has its own shared memory region for receiving
// Nodes connect to each other on-demand
#pragma once

#include <semaphore.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "nexus/registry/SharedMemoryRegistry.h"
#include "nexus/transport/LockFreeQueue.h"

namespace Nexus {
namespace rpc {

// Forward declaration
class NodeImpl;

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
    using ReceiveCallback = std::function<void(const uint8_t* data, size_t size, const std::string& from_node_id)>;
    using NodeEventCallback = std::function<void(const std::string& node_id, bool is_joined)>;

    // 🔧 跨进程通知机制
    enum class NotifyMechanism {
        CONDITION_VARIABLE,  // Condition Variable (传统方案，可靠)
        SEMAPHORE,           // POSIX Semaphore (推荐，低CPU高可靠)
        SMART_POLLING        // 智能轮询 (自旋+指数退避，超低延迟)
    };

    // Queue capacities in bytes (Variable-length RingBuffer)
    // Control Queue: 128KB (Enough for ~2000 small control messages)
    static constexpr size_t CONTROL_QUEUE_SIZE_BYTES = 16 * 1024;
    // Data Queue: 512KB (Enough for ~256 large 2KB messages or ~8000 small messages)
    static constexpr size_t DATA_QUEUE_SIZE_BYTES = 524288;

    static constexpr size_t QUEUE_CAPACITY = DATA_QUEUE_SIZE_BYTES;
    // 🔧 Process-Level Transport: Increase queue limit to support multiple nodes sharing same transport
    static constexpr size_t MAX_INBOUND_QUEUES = 64;  // Max senders to this process
    static constexpr size_t MAX_ACCESSORS = 128;      // Max clients connected to this transport

    struct Config {
        size_t queue_capacity;
        size_t max_inbound_queues;  // 可配置的队列数上限（不能超过MAX_INBOUND_QUEUES）
        bool enable_stats;
        bool auto_cleanup;
        NotifyMechanism notify_mechanism;  // 🔧 通知机制选择

        Config()
            : queue_capacity(QUEUE_CAPACITY),
              max_inbound_queues(MAX_INBOUND_QUEUES)
              ,
              enable_stats(true),
              auto_cleanup(true),
              notify_mechanism(NotifyMechanism::CONDITION_VARIABLE)  // 🔧 默认使用CONDITION_VARIABLE（最优方案）
        {}
    };

    SharedMemoryTransportV3();
    ~SharedMemoryTransportV3();

    /**
     * @brief Initialize transport
     * @param transport_id Unique identifier for the transport (usually process-based, e.g. "process_<PID>")
     * @param config Configuration
     * @return true if successful
     */
    bool initialize(const std::string& transport_id, const Config& config = Config());

    /**
     * @brief Register a specific node ID to this transport's shared memory
     * @param node_id The logical Node ID to register
     * @return true if successful
     */
    bool registerNodeToRegistry(const std::string& node_id);

    /**
     * @brief Unregister a specific node ID from this transport's shared memory
     * @param node_id The logical Node ID to unregister
     * @return true if successful
     */
    bool unregisterNodeFromRegistry(const std::string& node_id);

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
     * @brief Add receive callback for a specific node
     * @param node_id The Node ID registering the callback
     * @param callback Function to call when message arrives
     */
    void addReceiveCallback(const std::string& node_id, ReceiveCallback callback);

    /**
     * @brief Remove receive callback for a specific node
     * @param node_id The Node ID removing the callback
     */
    void removeReceiveCallback(const std::string& node_id);

    /**
     * @brief Add node event callback for a specific node
     * @param node_id The Node ID registering the callback
     * @param callback Function to call when node events occur
     */
    void addNodeEventCallback(const std::string& node_id, NodeEventCallback callback);

    /**
     * @brief Remove node event callback for a specific node
     * @param node_id The Node ID removing the callback
     */
    void removeNodeEventCallback(const std::string& node_id);

    /**
     * @brief Start receiving thread
     */
    void startReceiving();

    /**
     * @brief Stop receiving thread
     */
    void stopReceiving();

    /**
     * @brief Get transport ID
     */
    std::string getTransportId() const { return transport_id_; }



    /**
     * @brief Check if initialized
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief Disconnect from a specific node
     * @param target_node_id Node ID to disconnect from
     *
     * 当节点退出后重新加入时,需要断开旧连接以便重新建立新连接
     */
    void disconnectFromNode(const std::string& target_node_id);

    /**
     * @brief Get all local node IDs (nodes using shared memory)
     * @return Vector of active node IDs
     */
    std::vector<std::string> getLocalNodes() const;

    /**
     * @brief Get all registered node IDs across all processes
     * @return Vector of all node IDs registered in SharedMemoryRegistry
     */
    std::vector<std::string> getAllRegisteredNodes() const;

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
     * @brief Get the Shared Memory Name for a remote node (for optimization)
     * @param node_id The remote Node ID
     * @return The SHM name (e.g. process identifier) or empty if not found
     */
    std::string getRemoteShmName(const std::string& node_id);

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
    // 🔧 双队列架构：控制面与数据面分离
    struct InboundQueue {
        // 🔧 Store sender_id as atomic uint64_t array for true atomicity (64 bytes = 8 * uint64_t)
        std::atomic<uint64_t> sender_id_atomic[8];
        std::atomic<uint32_t> flags;  // Bit 0: valid, Bit 1: active

        // 🔧 控制队列（高优先级）：NODE_JOIN, SERVICE_REGISTER等
        std::atomic<uint32_t> control_pending;  // 待处理控制消息计数
        sem_t control_sem;                      // SEM模式: POSIX信号量
        char control_sem_padding[64 - sizeof(sem_t)];
        LockFreeRingBuffer<CONTROL_QUEUE_SIZE_BYTES> control_queue;

        // 🔧 数据队列（正常优先级）：普通数据消息
        std::atomic<uint32_t> data_pending;
        sem_t data_sem;
        char data_sem_padding[64 - sizeof(sem_t)];
        LockFreeRingBuffer<DATA_QUEUE_SIZE_BYTES> data_queue;

        // 🔧 流控：背压机制
        std::atomic<uint32_t> congestion_level;  // 拥塞等级 0-100
        std::atomic<uint64_t> drop_count;        // 累计丢包数

        char padding[64];  // Cache line alignment
    };

    // Node's shared memory layout
    struct alignas(64) NodeHeader {
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> version;
        std::atomic<uint32_t> num_queues;
        std::atomic<uint32_t> max_queues;
        std::atomic<uint64_t> last_heartbeat;
        std::atomic<bool> ready;         // 两阶段提交：节点是否完全初始化
        std::atomic<int32_t> owner_pid;  // 进程PID：用于检测进程是否存活

        // CRITICAL: 访问者PID数组 - 跟踪所有打开此共享内存的进程
        // 用于安全清理判断：只有当所有访问者都死亡时才能删除
        static constexpr int MAX_ACCESSORS = 64;
        std::atomic<int32_t> accessor_pids[MAX_ACCESSORS];
        std::atomic<uint32_t> num_accessors;  // 当前访问者数量

        // 全局共享CV：所有InboundQueue共享同一个cond_var
        pthread_mutex_t global_mutex;  // CV模式：全局互斥锁
        pthread_cond_t global_cond;    // CV模式：全局条件变量

        char padding[64];  // Cache line alignment
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

        RemoteConnection() : shm_ptr(nullptr), shm_fd(-1), my_queue(nullptr), connected(false) {}
    };

    static constexpr uint32_t MAGIC = 0x4C524E33;  // "LRN3" = LibRpc Node v3
    static constexpr uint32_t VERSION = 1;

    // Helper methods
    bool createMySharedMemory();
    void destroyMySharedMemory();
    bool connectToNode(const std::string& target_node_id);
    InboundQueue* findOrCreateQueue(NodeSharedMemory* remote_shm, const std::string& sender_id);
    void receiveLoop();
    void receiveLoop_Semaphore();  // 🔧 Semaphore模式的接收循环
    void receiveLoop_CV();         // Condition Variable模式的接收循环
    void heartbeatLoop();
    void cleanupStaleQueues();
    std::string generateShmName();

    // Accessor PID tracking helpers
    void addAccessor(pid_t pid);
    void removeAccessor(pid_t pid);
    static void addAccessorToNode(NodeHeader* header, pid_t pid);
    static void removeAccessorFromNode(NodeHeader* header, pid_t pid);
    static bool hasActiveAccessors(NodeHeader* header);

    // State
    std::string transport_id_;
    std::string my_shm_name_;
    Config config_;
    bool initialized_;
    NotifyMechanism notify_mechanism_;  // 当前使用的通知机制
    

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

    // Callbacks & Local Node Management
    std::unordered_map<std::string, ReceiveCallback> receive_callbacks_;
    std::unordered_map<std::string, NodeEventCallback> node_event_callbacks_;
    std::unordered_set<std::string> local_registered_nodes_;
    mutable std::mutex callbacks_mutex_;

    // Condition variable for queue availability (used when active_queues is empty)
    std::mutex queue_wait_mutex_;
    std::condition_variable queue_wait_cv_;
    std::atomic<bool> has_active_queues_{false};

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

}  // namespace rpc
}  // namespace Nexus
