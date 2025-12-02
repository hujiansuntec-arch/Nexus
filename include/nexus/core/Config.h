#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Nexus {
namespace rpc {

/**
 * @brief Centralized configuration for Nexus framework
 */
struct Config {
    // Node configuration
    struct NodeConfig {
        size_t max_inbound_queues = 32;      // Maximum inbound queues per node
        size_t queue_capacity = 256;          // Messages per queue
        size_t num_processing_threads = 4;   // Thread pool size
        size_t max_queue_size = 25000;       // Max messages per async queue
    } node;
    
    // Transport layer configuration
    struct TransportConfig {
        bool enable_udp = true;              // Enable UDP transport
        uint16_t udp_port_base = 47200;     // UDP port range start
        uint16_t udp_port_max = 47999;      // UDP port range end
    } transport;
    
    // Shared memory configuration
    struct SharedMemoryConfig {
        size_t queue_capacity = 256;         // Queue capacity (64-1024)
        size_t max_inbound_queues = 64;     // Hard limit (max 64)
        uint32_t heartbeat_interval_ms = 1000;  // 1 second
        uint32_t node_timeout_ms = 5000;        // 5 seconds
        size_t message_size = 2048;          // Max message size in bytes
    } shm;
    
    // Large data channel configuration
    struct LargeDataConfig {
        size_t buffer_size = 64 * 1024 * 1024;     // 64MB default
        size_t max_block_size = 8 * 1024 * 1024;   // 8MB max
    } large_data;
    
    /**
     * @brief Get global configuration instance (singleton)
     */
    static Config& instance();
    
    /**
     * @brief Load configuration from environment variables
     * 
     * Supported variables:
     * - NEXUS_MAX_INBOUND_QUEUES
     * - NEXUS_QUEUE_CAPACITY
     * - NEXUS_NUM_THREADS
     * - NEXUS_HEARTBEAT_INTERVAL_MS
     * - NEXUS_NODE_TIMEOUT_MS
     * - NEXUS_BUFFER_SIZE
     */
    void loadFromEnv();
    
    /**
     * @brief Calculate memory footprint based on current config
     */
    size_t calculateMemoryFootprint() const;
    
private:
    Config() = default;
};

} // namespace rpc
} // namespace Nexus
