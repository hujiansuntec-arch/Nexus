#include "nexus/core/Config.h"
#include <cstdlib>
#include <algorithm>

namespace Nexus {
namespace rpc {

// Helper function for C++14 compatibility (std::clamp is C++17)
namespace {
    template<typename T>
    constexpr const T& clamp(const T& val, const T& min, const T& max) {
        return val < min ? min : (val > max ? max : val);
    }
}

Config& Config::instance() {
    static Config config;
    return config;
}

void Config::loadFromEnv() {
    // Node configuration
    if (const char* val = std::getenv("NEXUS_MAX_INBOUND_QUEUES")) {
        node.max_inbound_queues = clamp(
            static_cast<size_t>(std::atoi(val)), 
            size_t(8), size_t(64)
        );
    }
    
    if (const char* val = std::getenv("NEXUS_QUEUE_CAPACITY")) {
        node.queue_capacity = clamp(
            static_cast<size_t>(std::atoi(val)), 
            size_t(64), size_t(1024)
        );
    }
    
    if (const char* val = std::getenv("NEXUS_NUM_THREADS")) {
        node.num_processing_threads = clamp(
            static_cast<size_t>(std::atoi(val)), 
            size_t(1), size_t(16)
        );
    }
    
    if (const char* val = std::getenv("NEXUS_MAX_QUEUE_SIZE")) {
        node.max_queue_size = static_cast<size_t>(std::atoi(val));
    }
    
    // Shared memory configuration
    if (const char* val = std::getenv("NEXUS_SHM_QUEUE_CAPACITY")) {
        shm.queue_capacity = clamp(
            static_cast<size_t>(std::atoi(val)), 
            size_t(64), size_t(1024)
        );
    }
    
    if (const char* val = std::getenv("NEXUS_HEARTBEAT_INTERVAL_MS")) {
        shm.heartbeat_interval_ms = static_cast<uint32_t>(std::atoi(val));
    }
    
    if (const char* val = std::getenv("NEXUS_NODE_TIMEOUT_MS")) {
        shm.node_timeout_ms = static_cast<uint32_t>(std::atoi(val));
    }
    
    // Large data configuration
    if (const char* val = std::getenv("NEXUS_BUFFER_SIZE")) {
        large_data.buffer_size = static_cast<size_t>(std::atoll(val));
    }
    
    if (const char* val = std::getenv("NEXUS_MAX_BLOCK_SIZE")) {
        large_data.max_block_size = static_cast<size_t>(std::atoll(val));
    }
}

size_t Config::calculateMemoryFootprint() const {
    // SharedMemory footprint: queues × capacity × message_size
    size_t shm_memory = node.max_inbound_queues * 
                        shm.queue_capacity * 
                        shm.message_size;
    
    // Large data channel footprint
    size_t large_data_memory = large_data.buffer_size;
    
    // Async queue footprint (rough estimate)
    size_t async_memory = node.num_processing_threads * 
                          node.max_queue_size * 
                          256;  // Estimated message size
    
    return shm_memory + large_data_memory + async_memory;
}

} // namespace rpc
} // namespace Nexus
