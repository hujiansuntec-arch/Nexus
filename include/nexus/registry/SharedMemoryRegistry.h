// Shared Memory Registry for Dynamic Node Management
// Manages node registration and discovery
#pragma once

#include <errno.h>
#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Nexus {
namespace rpc {

/**
 * @brief Node information in the registry
 */
struct NodeInfo {
    std::string node_id;
    std::string shm_name;  // Name of node's shared memory (e.g., "/librpc_node_12345")
    pid_t pid;
    uint64_t last_heartbeat;
    bool active;

    NodeInfo() : pid(0), last_heartbeat(0), active(false) {}
};

/**
 * @brief Shared memory registry for node discovery and management
 *
 * Architecture:
 * - Central registry at /dev/shm/librpc_registry
 * - Each node registers itself with a unique shared memory name
 * - Other nodes can discover all active nodes by reading the registry
 * - Heartbeat-based liveness detection
 */
class SharedMemoryRegistry {
public:
    static constexpr size_t MAX_REGISTRY_ENTRIES = 256;  // Support up to 256 nodes
    static constexpr size_t NODE_ID_SIZE = 64;
    static constexpr size_t SHM_NAME_SIZE = 64;

    SharedMemoryRegistry();
    ~SharedMemoryRegistry();

    /**
     * @brief Initialize registry (create or open)
     * @return true if successful
     */
    bool initialize();

    /**
     * @brief Register a node in the registry
     * @param node_id Unique node identifier
     * @param shm_name Shared memory name for this node
     * @return true if registered successfully
     */
    bool registerNode(const std::string& node_id, const std::string& shm_name);

    /**
     * @brief Unregister a node from the registry
     * @param node_id Node identifier to unregister
     * @return true if unregistered successfully
     */
    bool unregisterNode(const std::string& node_id);

    /**
     * @brief Update heartbeat for a node
     * @param node_id Node identifier
     * @return true if updated
     */
    bool updateHeartbeat(const std::string& node_id);

    /**
     * @brief Get all active nodes
     * @return Vector of active node information
     */
    std::vector<NodeInfo> getAllNodes() const;

    /**
     * @brief Find a specific node
     * @param node_id Node identifier to find
     * @param info Output node information
     * @return true if found
     */
    bool findNode(const std::string& node_id, NodeInfo& info) const;

    /**
     * @brief Check if a node exists
     * @param node_id Node identifier
     * @return true if node is registered and active
     */
    bool nodeExists(const std::string& node_id) const;

    /**
     * @brief Clean up stale nodes (timeout-based)
     * @param timeout_ms Timeout in milliseconds
     * @return Number of nodes cleaned up
     */
    int cleanupStaleNodes(uint64_t timeout_ms);

    /**
     * @brief Get number of active nodes
     * @return Count of active nodes
     */
    int getActiveNodeCount() const;

    /**
     * @brief Check if current process is the cleanup master
     * Election strategy: smallest PID among active processes
     * This ensures only ONE process performs cleanup, avoiding race conditions
     * @return true if this process should perform cleanup
     */
    bool amICleanupMaster() const;

    /**
     * @brief Check if initialized
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief Cleanup orphaned registry (static utility)
     * @return true if cleanup succeeded
     */
    static bool cleanupOrphanedRegistry();

    /**
     * @brief Force remove registry (for debugging/recovery)
     * WARNING: Only use when all processes are stopped!
     * @return true if removed
     */
    static bool forceRemoveRegistry();

private:
    // Shared memory structures

    struct RegistryEntry {
        std::atomic<uint32_t> flags;     // Bit 0: valid, Bit 1: active
        std::atomic<uint32_t> version;   // 🔧 Entry version to detect ABA problem
        std::atomic<uint32_t> pid;       // Process ID as atomic
        std::atomic<uint32_t> _padding;  // 🔧 Alignment padding
        std::atomic<uint64_t> last_heartbeat;

        // 🔧 Store node_id and shm_name as atomic uint64_t arrays for true atomicity
        // node_id: 64 bytes = 8 * uint64_t
        std::atomic<uint64_t> node_id_atomic[8];
        // shm_name: 64 bytes = 8 * uint64_t
        std::atomic<uint64_t> shm_name_atomic[8];

        char padding[48];  // 🔧 增加padding到48字节，确保每个entry独占cache line（总共224字节）
    };

    static_assert(sizeof(RegistryEntry) <= 256, "RegistryEntry size too large");

    struct alignas(64) RegistryHeader {
        pthread_mutex_t global_lock;  // 🔧 进程间互斥锁，保护所有 registry 访问
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> version;
        std::atomic<uint32_t> num_entries;
        std::atomic<uint32_t> capacity;
        std::atomic<uint32_t> ref_count;                       // 🔧 引用计数：有多少进程正在使用
        std::atomic<uint32_t> ref_pids[MAX_REGISTRY_ENTRIES];  // 🔧 记录正在使用的进程PID，与节点数一致
    };

    struct RegistryRegion {
        RegistryHeader header;
        RegistryEntry entries[MAX_REGISTRY_ENTRIES];
    };

    static constexpr uint32_t MAGIC = 0x4C525247;  // "LRRG" = LibRpc ReGistry
    static constexpr uint32_t VERSION = 1;
    static constexpr const char* REGISTRY_SHM_NAME = "/librpc_registry";

    // Helper methods
    int findEntryIndex(const std::string& node_id) const;

    uint64_t getCurrentTimeMs() const;
    bool isProcessAlive(pid_t pid) const;

    // 🔧 RAII lock helper for registry global lock
    class RegistryLock {
    public:
        explicit RegistryLock(pthread_mutex_t* mutex, int timeout_ms = 5000) : mutex_(mutex), locked_(false) {
            // 🔧 使用超时锁，防止永久阻塞
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            // 🔧 安全计算超时时间戳，避免纳秒溢出
            long timeout_sec = timeout_ms / 1000;
            long timeout_nsec = (timeout_ms % 1000) * 1000000;
            ts.tv_sec += timeout_sec;
            ts.tv_nsec += timeout_nsec;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += ts.tv_nsec / 1000000000;
                ts.tv_nsec %= 1000000000;
            }

            int result = pthread_mutex_timedlock(mutex_, &ts);
            if (result == EOWNERDEAD) {
                pthread_mutex_consistent(mutex_);
                locked_ = true;
            } else if (result == 0) {
                locked_ = true;
            } else if (result == ETIMEDOUT) {
                // 超时，可能有进程死锁
            }
        }

        ~RegistryLock() {
            if (locked_) {
                int result = pthread_mutex_unlock(mutex_);
                if (result != 0) {
                    // Cannot throw in destructor, but log the error
                    // This indicates serious mutex corruption
                }
            }
        }

        bool isLocked() const { return locked_; }

    private:
        pthread_mutex_t* mutex_;
        bool locked_;
        RegistryLock(const RegistryLock&) = delete;
        RegistryLock& operator=(const RegistryLock&) = delete;
    };

public:
    // 🔧 Atomic string helpers (made public for use in transport layer)
    static void writeAtomicString(std::atomic<uint64_t>* atomic_array, const std::string& str, size_t max_bytes);
    static std::string readAtomicString(const std::atomic<uint64_t>* atomic_array, size_t max_bytes);

private:
    // State
    bool initialized_;
    void* shm_ptr_;
    int shm_fd_;
    RegistryRegion* registry_;
};

}  // namespace rpc
}  // namespace Nexus
