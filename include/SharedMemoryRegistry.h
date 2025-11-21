// Shared Memory Registry for Dynamic Node Management
// Manages node registration and discovery
#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <sys/types.h>

namespace librpc {

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
     * @brief Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    /**
     * @brief Cleanup orphaned registry (static utility)
     * @return true if cleanup succeeded
     */
    static bool cleanupOrphanedRegistry();
    
private:
    // Shared memory structures
    
    struct RegistryEntry {
        std::atomic<uint32_t> flags;  // Bit 0: valid, Bit 1: active
        char node_id[NODE_ID_SIZE];
        char shm_name[SHM_NAME_SIZE];
        pid_t pid;
        std::atomic<uint64_t> last_heartbeat;
        char padding[28];  // Padding for cache alignment (total 168 bytes)
    };
    
    static_assert(sizeof(RegistryEntry) <= 192, "RegistryEntry size too large");
    
    struct alignas(64) RegistryHeader {
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> version;
        std::atomic<uint32_t> num_entries;
        std::atomic<uint32_t> capacity;
        char padding[48];
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
    int findFreeEntryIndex() const;
    uint64_t getCurrentTimeMs() const;
    bool isProcessAlive(pid_t pid) const;
    
    // State
    bool initialized_;
    void* shm_ptr_;
    int shm_fd_;
    RegistryRegion* registry_;
};

} // namespace librpc
