#pragma once

#include <stdint.h>
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace Nexus {
namespace rpc {

// Forward declaration
class LargeDataChannel;

/**
 * @brief Transport type enumeration
 */
enum class TransportType {
    INPROCESS = 0,     // In-process (same process)
    SHARED_MEMORY,     // Shared memory (local inter-process)
    UDP                // UDP (remote inter-process)
};

/**
 * @brief Service type enumeration
 */
enum class ServiceType {
    ALL = 0,           // All service types (for query)
    NORMAL_MESSAGE,    // Normal pub/sub messages (256B-2KB)
    LARGE_DATA         // Large data channel (1MB-8MB)
};

/**
 * @brief Service descriptor
 */
struct ServiceDescriptor {
    std::string node_id;        // Node providing this service
    std::string group;          // Message group
    std::string topic;          // Topic name
    ServiceType type;           // Service type
    std::string channel_name;   // Large data channel name (empty for normal messages)
    TransportType transport;    // Transport type (INPROCESS/SHARED_MEMORY/UDP)
    std::string udp_address;    // UDP address (IP:port, empty for non-UDP)
    
    ServiceDescriptor() 
        : type(ServiceType::NORMAL_MESSAGE)
        , transport(TransportType::INPROCESS) {}
    
    // Get unique capability identifier
    std::string getCapability() const {
        if (type == ServiceType::LARGE_DATA) {
            return group + "/" + channel_name + "/" + topic;
        } else {
            return group + "/" + topic;
        }
    }
    
    // Get unique service key (node + capability + transport)
    std::string getServiceKey() const {
        return node_id + ":" + getCapability() + ":" + std::to_string(static_cast<int>(transport));
    }
};

/**
 * @brief Service event type
 */
enum class ServiceEvent {
    SERVICE_ADDED,      // New service registered
    SERVICE_REMOVED,    // Service unregistered
    NODE_JOINED,        // New node joined
    NODE_LEFT           // Node left
};

/**
 * @brief Service discovery callback
 * Called when service changes are detected
 * @param event Service event type
 * @param service Service descriptor
 */
using ServiceDiscoveryCallback = std::function<void(ServiceEvent event, 
                                                     const ServiceDescriptor& service)>;

/**
 * @brief Message queue overflow policy (for normal pub/sub messages)
 */
enum class QueueOverflowPolicy {
    DROP_OLDEST,    // Drop oldest message (default, suitable for sensor data)
    DROP_NEWEST,    // Drop newest message (suitable for control commands)
    BLOCK           // Block until space available (may cause deadlock)
};

/**
 * @brief Message queue overflow callback (for normal pub/sub messages)
 * Called when queue is full and a message is dropped
 * @param msg_group Message group
 * @param topic Topic name
 * @param dropped_count Total dropped messages so far
 */
using QueueOverflowCallback = std::function<void(const std::string& msg_group,
                                                   const std::string& topic,
                                                   size_t dropped_count)>;

/**
 * @brief Node interface for peer-to-peer communication
 * 
 * Features:
 * - Subscribe to topics within message groups
 * - Publish messages to subscribers
 * - Support both in-process and inter-process communication
 * - Multiple nodes can coexist in the same process
 * - Large data transfer channel (for high-frequency big data transmission)
 */
class Node {
public:
    using Property = std::string;
    using Callback = std::function<void(const Property& msg_group, 
                                       const Property& topic, 
                                       const uint8_t* payload, 
                                       size_t size)>;

    enum Error {
        NO_ERROR         = 0,
        INVALID_ARG      = 1,
        NOT_INITIALIZED  = 2,
        ALREADY_EXISTS   = 3,
        NOT_FOUND        = 4,
        NETWORK_ERROR    = 5,
        TIMEOUT          = 6,
        QUEUE_FULL       = 7,
        UNEXPECTED_ERROR = 99,
    };

    virtual ~Node() = default;

    /**
     * @brief Publish a message to all subscribers of a topic
     * @param msg_group Message group name
     * @param topic Topic name within the group
     * @param payload Message payload data
     * @return Error code
     */
    virtual Error publish(const Property& msg_group, 
                          const Property& topic, 
                          const Property& payload) = 0;

    /**
     * @brief Subscribe to topics within a message group
     * @param msg_group Message group name
     * @param topics List of topic names to subscribe
     * @param callback Callback function to receive messages
     * @return Error code
     */
    virtual Error subscribe(const Property& msg_group, 
                          const std::vector<Property>& topics, 
                          const Callback& callback) = 0;

    /**
     * @brief Unsubscribe from topics within a message group
     * @param msg_group Message group name
     * @param topics List of topic names to unsubscribe
     * @return Error code
     */
    virtual Error unsubscribe(const Property& msg_group, 
                            const std::vector<Property>& topics) = 0;

    /**
     * @brief Check if subscribed to a specific topic
     * @param msg_group Message group name
     * @param topic Topic name
     * @return true if subscribed
     */
    virtual bool isSubscribed(const Property& msg_group, 
                             const Property& topic) const = 0;
    
    /**
     * @brief Send large data with simplified API
     * 
     * This is a high-level API that handles all the details:
     * - Automatically creates/manages the channel
     * - Writes data to the ring buffer
     * - Sends notification to subscribers
     * - Automatic cleanup when no longer needed
     * 
     * Large data channel features:
     * - Zero-copy shared memory ring buffer
     * - High throughput (~135 MB/s for 1MB blocks)
     * - Support for data up to 8MB per block
     * - Automatic memory management (MAP_NORESERVE)
     * - Auto-cleanup on process exit (引用计数)
     * - Startup cleanup for orphaned channels
     * 
     * Usage:
     *   node->sendLargeData("sensor", "video_channel", "frame_001", data, size);
     * 
     * @param msg_group Message group for notification (subscribers should subscribe to this group)
     * @param channel_name Name of the large data channel (internal identifier)
     * @param topic Topic name for this data
     * @param data Pointer to data buffer
     * @param size Size of data in bytes (up to 8MB)
     * @return NO_ERROR on success, TIMEOUT if buffer full, error code otherwise
     */
    virtual Error sendLargeData(const std::string& msg_group,
                               const std::string& channel_name,
                               const std::string& topic,
                               const uint8_t* data,
                               size_t size) = 0;
    
    /**
     * @brief Get or create a large data channel for reading/writing
     * 
     * This method provides access to the underlying large data channel.
     * Use this when you need direct control over the channel, such as:
     * - Receiver side: Reading data after receiving notification
     * - Advanced scenarios: Manual buffer management
     * 
     * The channel is automatically created if it doesn't exist.
     * 
     * @param channel_name Name of the large data channel
     * @return Shared pointer to LargeDataChannel, or nullptr on error
     */
    virtual std::shared_ptr<LargeDataChannel> getLargeDataChannel(
        const std::string& channel_name) = 0;
    
    /**
     * @brief Set message queue overflow policy (for normal pub/sub messages)
     * @param policy Overflow policy (default: DROP_OLDEST)
     */
    virtual void setQueueOverflowPolicy(QueueOverflowPolicy policy) = 0;
    
    /**
     * @brief Set message queue overflow callback (for normal pub/sub messages)
     * @param callback Callback function to be called when queue is full
     */
    virtual void setQueueOverflowCallback(QueueOverflowCallback callback) = 0;
    
    /**
     * @brief Cleanup orphaned shared memory channels
     * Should be called periodically or at startup
     * @return Number of cleaned channels
     */
    virtual size_t cleanupOrphanedChannels() = 0;
    
    /**
     * @brief Discover services in the network
     * @param group Message group to filter (empty for all)
     * @param type Service type to filter (ALL for all types)
     * @return Vector of service descriptors
     */
    virtual std::vector<ServiceDescriptor> discoverServices(
        const std::string& group = "",
        ServiceType type = ServiceType::ALL) = 0;
    
    /**
     * @brief Find nodes providing a specific capability
     * @param capability Capability string (e.g., "sensor/temperature")
     * @return Vector of node IDs
     */
    virtual std::vector<std::string> findNodesByCapability(
        const std::string& capability) = 0;
    
    /**
     * @brief Find large data channels
     * @param group Message group to filter (empty for all)
     * @return Vector of large data service descriptors
     */
    virtual std::vector<ServiceDescriptor> findLargeDataChannels(
        const std::string& group = "") = 0;
    
    /**
     * @brief Set service discovery callback
     * @param callback Callback to be invoked on service changes
     */
    virtual void setServiceDiscoveryCallback(ServiceDiscoveryCallback callback) = 0;
};

/**
 * @brief Transport mode for inter-process communication
 */
enum class TransportMode {
    AUTO,           // Automatic selection (default: lock-free shared memory if available)
    LOCK_FREE_SHM,  // Lock-free shared memory (high performance, recommended)
    UDP             // UDP transport (for distributed scenarios)
};

/**
 * @brief Create a new Node instance
 * 
 * The node automatically supports both in-process and inter-process communication:
 * - In-process: Direct function calls (zero-copy, <1μs latency)
 * - Inter-process: Shared memory (lock-free SPSC queues, ~10000 msg/s) or UDP
 * 
 * The framework automatically determines the communication method when publishing:
 * - If target nodes are in the same process → in-process delivery
 * - If target nodes are in other processes → shared memory or UDP delivery
 * 
 * @param node_id Unique identifier for this node (optional, auto-generated if empty)
 * @param mode Transport mode for inter-process communication
 * @return Shared pointer to Node instance
 */
std::shared_ptr<Node> createNode(const std::string& node_id = "", 
                                 TransportMode mode = TransportMode::AUTO);

} // namespace rpc
} // namespace Nexus
