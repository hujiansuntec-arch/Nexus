#pragma once

#include "nexus/core/Node.h"
#include "nexus/core/Message.h"
#include "nexus/transport/LargeDataChannel.h"
#include <map>
#include <set>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <vector>

namespace Nexus {
namespace rpc {

// Forward declarations
class UdpTransport;
class SharedMemoryTransportV3;

/**
 * @brief Node implementation supporting both in-process and inter-process communication
 */
class NodeImpl : public Node, public std::enable_shared_from_this<NodeImpl> {
    // Friend class to allow SharedMemoryTransportV3 to call handleNodeEvent
    friend class SharedMemoryTransportV3;
    
public:
    static constexpr size_t NUM_PROCESSING_THREADS = 4;  // Thread pool size (for QueueStats)
    
    NodeImpl(const std::string& node_id, bool use_udp, uint16_t udp_port, 
             TransportMode transport_mode = TransportMode::AUTO);
    ~NodeImpl() override;

    // Initialize after construction (must be called after shared_ptr is created)
    void initialize(uint16_t udp_port);

    // Node interface implementation
    Error publish(const Property& msg_group, 
                   const Property& topic, 
                   const Property& payload) override;

    Error subscribe(const Property& msg_group, 
                   const std::vector<Property>& topics, 
                   const Callback& callback) override;

    Error unsubscribe(const Property& msg_group, 
                     const std::vector<Property>& topics) override;

    bool isSubscribed(const Property& msg_group, 
                     const Property& topic) const override;
    
    // Large data channel support
    Error sendLargeData(const std::string& msg_group,
                       const std::string& channel_name,
                       const std::string& topic,
                       const uint8_t* data,
                       size_t size) override;
    
    std::shared_ptr<LargeDataChannel> getLargeDataChannel(
        const std::string& channel_name) override;
    
    // Queue overflow management
    void setQueueOverflowPolicy(QueueOverflowPolicy policy) override;
    void setQueueOverflowCallback(QueueOverflowCallback callback) override;
    
    // Cleanup orphaned channels
    size_t cleanupOrphanedChannels() override;
    
    // Service discovery
    std::vector<ServiceDescriptor> discoverServices(
        const std::string& group = "",
        ServiceType type = ServiceType::ALL) override;
    
    std::vector<std::string> findNodesByCapability(
        const std::string& capability) override;
    
    std::vector<ServiceDescriptor> findLargeDataChannels(
        const std::string& group = "") override;
    
    void setServiceDiscoveryCallback(ServiceDiscoveryCallback callback) override;
    
    // Performance statistics
    struct QueueStats {
        size_t queue_depth[NUM_PROCESSING_THREADS];  // Current depth per thread
        size_t total_dropped;                         // Total dropped messages
    };
    QueueStats getQueueStats() const;

    // Internal methods
    std::string getNodeId() const { return node_id_; }
    uint16_t getUdpPort() const;
    
private:
    // Message handling
    void handleMessage(const std::string& source_node_id,
                      const std::string& group,
                      const std::string& topic,
                      const uint8_t* payload,
                      size_t payload_len);
    
    void handleSubscribe(const std::string& remote_node_id,
                        uint16_t remote_port,
                        const std::string& remote_addr,
                        const std::string& group,
                        const std::string& topic);
    
    void handleUnsubscribe(const std::string& remote_node_id,
                          const std::string& group,
                          const std::string& topic);
    
    // Query existing nodes for their subscriptions (called when a new node joins)
    void queryExistingSubscriptions();
    
    // Handle subscription query from a new node
    void handleQuerySubscriptions(const std::string& remote_node_id,
                                 uint16_t remote_port,
                                 const std::string& remote_addr);
    
    // Handle subscription reply from existing nodes
    void handleSubscriptionReply(const std::string& remote_node_id,
                                uint16_t remote_port,
                                const std::string& remote_addr,
                                const std::string& group,
                                const std::string& topic);
    
    // In-process delivery
    void deliverInProcess(const std::string& group,
                         const std::string& topic,
                         const uint8_t* payload,
                         size_t payload_len);
    
    // Inter-process delivery (via shared memory or UDP)
    void deliverInterProcess(const std::vector<uint8_t>& packet,
                            const std::string& group,
                            const std::string& topic);
    
    // Send subscription registration to all nodes
    void broadcastSubscription(const std::string& group,
                              const std::string& topic,
                              bool is_subscribe);
    
    // Async message processing
    struct PendingMessage {
        std::string source_node_id;
        std::string group;
        std::string topic;
        std::vector<uint8_t> payload;
    };
    
    void messageProcessingThread(size_t thread_id);
    void enqueueMessage(const std::string& source_node_id,
                       const std::string& group,
                       const std::string& topic,
                       const uint8_t* payload,
                       size_t payload_len);
    
    // Subscription management
    struct SubscriptionInfo {
        std::set<std::string> topics;  // Topics within this group
        Callback callback;             // Callback for this group
    };
    
    // Remote node subscription info (UDP nodes)
    struct RemoteNodeInfo {
        std::string node_id;
        std::string address;
        uint16_t port;
        std::set<SubscriptionKey> subscriptions;  // What this remote node subscribed to
        std::chrono::steady_clock::time_point last_heartbeat;  // Last heartbeat time
    };
    
    // UDP heartbeat management
    void startUdpHeartbeat();  // Start UDP heartbeat thread
    void stopUdpHeartbeat();   // Stop UDP heartbeat thread
    void udpHeartbeatThread(); // UDP heartbeat worker thread
    void sendUdpHeartbeat();   // Send heartbeat to all UDP nodes
    void handleUdpHeartbeat(const std::string& from_node, const std::string& from_addr, uint16_t from_port);
    void checkUdpTimeouts();   // Check and clean up timed-out UDP nodes
    
private:
    std::string node_id_;
    bool use_udp_;
    TransportMode transport_mode_;
    std::atomic<bool> running_;
    
    // Local subscriptions: group -> SubscriptionInfo
    mutable std::mutex subscriptions_mutex_;
    std::map<std::string, SubscriptionInfo> subscriptions_;
    
    // Remote nodes registry: node_id -> RemoteNodeInfo
    mutable std::mutex remote_nodes_mutex_;
    std::map<std::string, RemoteNodeInfo> remote_nodes_;
    
    // Transport layers
    std::unique_ptr<UdpTransport> udp_transport_;                   // For remote communication
    std::unique_ptr<SharedMemoryTransportV3> shm_transport_v3_;     // For local communication (dynamic)
    
    // Large data channels: channel_name -> LargeDataChannel
    mutable std::mutex large_channels_mutex_;
    std::map<std::string, std::shared_ptr<LargeDataChannel>> large_channels_;
    
    // Service discovery helpers
    void registerService(const ServiceDescriptor& svc);
    void unregisterService(const ServiceDescriptor& svc);
    void broadcastServiceUpdate(const ServiceDescriptor& svc, bool is_add);
    void broadcastNodeEvent(bool is_joined);  // Broadcast NODE_JOIN/NODE_LEAVE event
    void queryRemoteServices();  // Query services from remote nodes at startup
    void handleServiceUpdate(const std::string& from_node,
                           const ServiceDescriptor& svc,
                           bool is_add);
    void handleServiceMessage(const std::string& from_node,
                            const std::string& group,
                            const std::string& topic,
                            const uint8_t* payload,
                            size_t payload_len,
                            bool is_register);
    void handleNodeEvent(const std::string& from_node, bool is_joined);
    void notifyNodeEvent(ServiceEvent event, const std::string& node_id);
    
    // Async message processing
    static constexpr size_t MAX_QUEUE_SIZE = 25000;       // Max messages per queue (increased for high throughput)
    mutable std::mutex message_queue_mutexes_[NUM_PROCESSING_THREADS];
    std::condition_variable message_queue_cvs_[NUM_PROCESSING_THREADS];
    std::queue<PendingMessage> message_queues_[NUM_PROCESSING_THREADS];  // One queue per thread
    std::vector<std::thread> processing_threads_;
    std::atomic<size_t> dropped_messages_{0};  // Counter for dropped messages due to queue overflow
    
    // Queue overflow policy
    QueueOverflowPolicy overflow_policy_{QueueOverflowPolicy::DROP_OLDEST};
    QueueOverflowCallback overflow_callback_;
    std::mutex overflow_callback_mutex_;
    
    // Background cleanup thread
    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};
    void cleanupThreadFunc();
    
    // UDP heartbeat thread
    std::thread udp_heartbeat_thread_;
    std::atomic<bool> udp_heartbeat_running_{false};
    static constexpr int UDP_HEARTBEAT_INTERVAL_MS = 1000;  // 1 second
    static constexpr int UDP_TIMEOUT_MS = 5000;             // 5 seconds
    
    // Service discovery state
    mutable std::mutex capabilities_mutex_;
    std::set<std::string> capabilities_;  // This node's capabilities (auto-registered)
    ServiceDiscoveryCallback service_discovery_callback_;
    std::mutex service_callback_mutex_;
    
    // Note: Global registries moved to Nexus::rpc::GlobalRegistry (see nexus/registry/GlobalRegistry.h)
    // This eliminates static members and improves testability
    
    // Register/unregister this node
    void registerNode();
    void unregisterNode();
    
    // Get all nodes (including this one)
    static std::vector<std::shared_ptr<NodeImpl>> getAllNodes();
};

} // namespace rpc
} // namespace Nexus
