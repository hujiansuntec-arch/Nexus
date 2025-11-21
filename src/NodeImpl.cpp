#include "NodeImpl.h"
#include "UdpTransport.h"
#include "SharedMemoryTransportV3.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iostream>

namespace librpc {

// Port range constants for node discovery
static constexpr uint16_t PORT_BASE = 47200;
static constexpr uint16_t PORT_MAX = 47999;
static constexpr uint16_t PORT_COUNT = PORT_MAX - PORT_BASE + 1;  // 800 ports

// Static members
std::mutex NodeImpl::registry_mutex_;
std::map<std::string, std::weak_ptr<NodeImpl>> NodeImpl::node_registry_;

// Generate unique node ID
static std::string generateNodeId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    std::stringstream ss;
    ss << "node_" << std::hex << std::setfill('0') << std::setw(12) << ms;
    return ss.str();
}

NodeImpl::NodeImpl(const std::string& node_id, bool use_udp, uint16_t udp_port,
                   TransportMode transport_mode)
    : node_id_(node_id.empty() ? generateNodeId() : node_id)
    , use_udp_(use_udp)
    , transport_mode_(transport_mode)
    , running_(true) {
    
    // UDP transport initialization will be done in a separate init method
}

NodeImpl::~NodeImpl() {
    running_ = false;
    
    // Wake up all processing threads
    for (auto& cv : message_queue_cvs_) {
        cv.notify_all();
    }
    
    // Wait for processing threads
    for (auto& thread : processing_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // Shutdown UDP transport (only if enabled)
    if (use_udp_ && udp_transport_) {
        udp_transport_->shutdown();
    }
    
    // Unregister this node
    unregisterNode();
}

// Initialization method to be called after construction
void NodeImpl::initialize(uint16_t udp_port) {
    // Register this node in global registry
    registerNode();
    
    // Start message processing threads
    for (size_t i = 0; i < NUM_PROCESSING_THREADS; ++i) {
        processing_threads_.emplace_back(&NodeImpl::messageProcessingThread, this, i);
    }
    
    // Initialize lock-free shared memory transport
    if (transport_mode_ == TransportMode::AUTO || transport_mode_ == TransportMode::LOCK_FREE_SHM) {
        shm_transport_v3_ = std::make_unique<SharedMemoryTransportV3>();
        if (!shm_transport_v3_->initialize(node_id_)) {
            std::cerr << "[LibRPC] Lock-free shared memory initialization failed" << std::endl;
            shm_transport_v3_.reset();
        } else {
            std::cout << "[LibRPC] Using lock-free shared memory transport (V3 - Dynamic)" << std::endl;
            // Set receive callback - parse MessagePacket format
            shm_transport_v3_->setReceiveCallback([this](const uint8_t* data, size_t size, 
                                                         const std::string& from) {
                // Data should be in MessagePacket format
                if (size < sizeof(MessagePacket)) {
                    return;
                }
                
                const MessagePacket* packet = reinterpret_cast<const MessagePacket*>(data);
                if (!packet->isValid()) {
                    return;
                }
                
                std::string source_node(packet->node_id);
                
                // Skip our own messages (critical: avoid self-reception)
                if (source_node == node_id_) {
                    return;
                }
                
                std::string group = packet->group_len > 0 ? 
                    std::string(packet->getGroup(), packet->group_len) : "";
                std::string topic = packet->topic_len > 0 ? 
                    std::string(packet->getTopic(), packet->topic_len) : "";
                
                MessageType msg_type = static_cast<MessageType>(packet->msg_type);
                
                switch (msg_type) {
                    case MessageType::SUBSCRIBE:
                        handleSubscribe(source_node, 0, "shm", group, topic);
                        break;
                        
                    case MessageType::UNSUBSCRIBE:
                        handleUnsubscribe(source_node, group, topic);
                        break;
                        
                    case MessageType::DATA:
                        handleMessage(source_node, group, topic, 
                                    packet->getPayload(), packet->payload_len);
                        break;
                        
                    case MessageType::QUERY_SUBSCRIPTIONS:
                        handleQuerySubscriptions(source_node, 0, "shm");
                        break;
                        
                    case MessageType::SUBSCRIPTION_REPLY:
                        handleSubscriptionReply(source_node, 0, "shm", group, topic);
                        break;
                }
            });
            shm_transport_v3_->startReceiving();
            
            // Query existing nodes for their subscriptions
            auto query_packet = MessageBuilder::build(node_id_, "", "", "", 
                                                     getUdpPort(), MessageType::QUERY_SUBSCRIPTIONS);
            shm_transport_v3_->broadcast(query_packet.data(), query_packet.size());
        }
    }
    
    // Initialize UDP transport if enabled
    if (use_udp_) {
        // Create callback handler (shared between both sockets)
        auto receiveCallback = [this](const uint8_t* data, size_t size, const std::string& from_addr) {
            // Parse and handle received message
            if (size < sizeof(MessagePacket)) {
                return;
            }
            
            const MessagePacket* packet = reinterpret_cast<const MessagePacket*>(data);
            if (!packet->isValid()) {
                return;
            }
            
            // Extract message components
            std::string source_node(packet->node_id);
            std::string group(packet->getGroup(), packet->group_len);
            std::string topic(packet->getTopic(), packet->topic_len);
            uint16_t sender_port = packet->udp_port;
            MessageType msg_type = static_cast<MessageType>(packet->msg_type);
            
            // Skip our own messages
            if (source_node == node_id_) {
                return;
            }
            
            // Handle based on message type
            switch (msg_type) {
                case MessageType::SUBSCRIBE:
                    handleSubscribe(source_node, sender_port, from_addr, group, topic);
                    break;
                    
                case MessageType::UNSUBSCRIBE:
                    handleUnsubscribe(source_node, group, topic);
                    break;
                    
                case MessageType::DATA:
                    handleMessage(source_node, group, topic, 
                                packet->getPayload(), packet->payload_len);
                    break;
                    
                case MessageType::QUERY_SUBSCRIPTIONS:
                    // Another node is asking for our subscriptions
                    handleQuerySubscriptions(source_node, sender_port, from_addr);
                    break;
                    
                case MessageType::SUBSCRIPTION_REPLY:
                    // Received subscription info from an existing node
                    handleSubscriptionReply(source_node, sender_port, from_addr,
                                           group, topic);
                    break;
            }
        };
        
        // Initialize main UDP socket (for all messages)
        // Use a fixed base port range (47200-47999) for easier discovery (800 ports)
        udp_transport_ = std::make_unique<UdpTransport>();
        
        // Try ports in our scan range first
        uint16_t target_port = udp_port;
        bool bound = false;
        
        if (target_port == 0) {
            // Auto-select from our known range (47200-47999)
            // Use random starting point to reduce collision in multi-process scenarios
            static std::atomic<uint16_t> next_port{0};
            
            // Initialize on first use with a random offset to spread allocations
            uint16_t current = next_port.load();
            if (current == 0) {
                auto now = std::chrono::high_resolution_clock::now();
                uint16_t random_offset = static_cast<uint16_t>(
                    now.time_since_epoch().count() % 800);
                next_port.store(PORT_BASE + random_offset);
            }
            
            // Try up to 800 ports in the range (47200-47999)
            for (int attempts = 0; attempts < PORT_COUNT && !bound; attempts++) {
                target_port = next_port.fetch_add(1);
                
                // Wrap around if we exceed the range
                if (target_port > PORT_MAX) {
                    // Reset to base and try again
                    uint16_t expected = target_port;
                    next_port.compare_exchange_strong(expected, PORT_BASE);
                    target_port = next_port.fetch_add(1);
                }
                
                bound = udp_transport_->initialize(target_port);
            }
            
            // If still not bound, let system choose
            if (!bound) {
                bound = udp_transport_->initialize(0);
            }
        } else {
            // User specified port
            bound = udp_transport_->initialize(target_port);
        }
        
        if (!bound) {
            // Failed to initialize
            return;
        }
        udp_transport_->setReceiveCallback(receiveCallback);
        
        // After UDP transport is ready, query existing nodes for their subscriptions
        // Use port scanning to discover nodes on localhost
        queryExistingSubscriptions();
    }
}

Node::Error NodeImpl::broadcast(const Property& msg_group, 
                                const Property& topic, 
                                const Property& payload) {
    if (msg_group.empty() || topic.empty()) {
        return Error::INVALID_ARG;
    }
    
    if (!running_) {
        return Error::NOT_INITIALIZED;
    }
    
    // Build message packet
    auto packet = MessageBuilder::build(node_id_, msg_group, topic, payload, 
                                       getUdpPort(), MessageType::DATA);
    
    // Deliver to in-process subscribers
    deliverInProcess(msg_group, topic, 
                    reinterpret_cast<const uint8_t*>(payload.data()), 
                    payload.size());
    
    // Deliver to inter-process subscribers (via shared memory or UDP)
    deliverInterProcess(packet, msg_group, topic);
    
    return Error::NO_ERROR;
}

Node::Error NodeImpl::subscribe(const Property& msg_group, 
                                const std::vector<Property>& topics, 
                                const Callback& callback) {
    if (msg_group.empty() || topics.empty() || !callback) {
        return Error::INVALID_ARG;
    }
    
    if (!running_) {
        return Error::NOT_INITIALIZED;
    }
    
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    // Get or create subscription info for this group
    auto& sub_info = subscriptions_[msg_group];
    
    // Add topics
    for (const auto& topic : topics) {
        if (!topic.empty()) {
            sub_info.topics.insert(topic);
        }
    }
    
    // Update callback
    sub_info.callback = callback;
    
    // Broadcast subscription to remote nodes via UDP
    for (const auto& topic : topics) {
        if (!topic.empty()) {
            broadcastSubscription(msg_group, topic, true);
        }
    }
    
    return Error::NO_ERROR;
}

Node::Error NodeImpl::unsubscribe(const Property& msg_group, 
                                  const std::vector<Property>& topics) {
    if (msg_group.empty()) {
        return Error::INVALID_ARG;
    }
    
    if (!running_) {
        return Error::NOT_INITIALIZED;
    }
    
    // Collect topics to broadcast unsubscribe (avoid broadcasting inside lock)
    std::vector<std::string> topics_to_broadcast;
    
    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        
        auto it = subscriptions_.find(msg_group);
        if (it == subscriptions_.end()) {
            return Error::NOT_FOUND;
        }
        
        if (topics.empty()) {
            // Remove entire group - collect all topics
            topics_to_broadcast.assign(it->second.topics.begin(), it->second.topics.end());
            subscriptions_.erase(it);
        } else {
            // Remove specific topics
            for (const auto& topic : topics) {
                auto topic_it = it->second.topics.find(topic);
                if (topic_it != it->second.topics.end()) {
                    topics_to_broadcast.push_back(topic);
                    it->second.topics.erase(topic_it);
                }
            }
            
            // Remove group if no topics left
            if (it->second.topics.empty()) {
                subscriptions_.erase(it);
            }
        }
    }
    
    // Broadcast unsubscribe for collected topics (outside of lock)
    for (const auto& topic : topics_to_broadcast) {
        broadcastSubscription(msg_group, topic, false);
    }
    
    return Error::NO_ERROR;
}

std::vector<std::pair<Node::Property, std::vector<Node::Property>>> 
NodeImpl::getSubscriptions() const {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    std::vector<std::pair<Property, std::vector<Property>>> result;
    result.reserve(subscriptions_.size());
    
    for (const auto& pair : subscriptions_) {
        std::vector<Property> topics(pair.second.topics.begin(), pair.second.topics.end());
        result.emplace_back(pair.first, std::move(topics));
    }
    
    return result;
}

bool NodeImpl::isSubscribed(const Property& msg_group, const Property& topic) const {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    
    auto it = subscriptions_.find(msg_group);
    if (it == subscriptions_.end()) {
        return false;
    }
    
    return it->second.topics.find(topic) != it->second.topics.end();
}

uint16_t NodeImpl::getUdpPort() const {
    if (use_udp_ && udp_transport_) {
        return udp_transport_->getPort();
    }
    return 0;
}

void NodeImpl::handleMessage(const std::string& source_node_id,
                            const std::string& group,
                            const std::string& topic,
                            const uint8_t* payload,
                            size_t payload_len) {
    // Enqueue message for async processing instead of blocking receive thread
    enqueueMessage(source_node_id, group, topic, payload, payload_len);
}

void NodeImpl::deliverInProcess(const std::string& group,
                               const std::string& topic,
                               const uint8_t* payload,
                               size_t payload_len) {
    // Get all registered nodes
    auto nodes = getAllNodes();
    
    // Deliver to each node (except ourselves)
    for (const auto& node : nodes) {
        if (node && node.get() != this) {
            node->handleMessage(node_id_, group, topic, payload, payload_len);
        }
    }
}

void NodeImpl::deliverInterProcess(const std::vector<uint8_t>& packet,
                                   const std::string& group,
                                   const std::string& topic) {
    // Build a set of local node IDs (in-process nodes)
    std::set<std::string> local_node_ids;
    {
        std::lock_guard<std::mutex> registry_lock(registry_mutex_);
        for (const auto& pair : node_registry_) {
            if (pair.second.lock()) {
                local_node_ids.insert(pair.first);
            }
        }
    }
    
    // Build a set of shared memory nodes (local inter-process nodes)
    std::set<std::string> shm_node_ids;
    if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
        // Lock-free transport: query active nodes
        auto shm_nodes = shm_transport_v3_->getLocalNodes();
        shm_node_ids.insert(shm_nodes.begin(), shm_nodes.end());
    }
    
    // Copy remote nodes info to avoid holding lock during network I/O
    std::vector<RemoteNodeInfo> target_nodes;
    {
        std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
        
        // Send to remote nodes that subscribed to this group/topic
        SubscriptionKey key(group, topic);
        for (const auto& pair : remote_nodes_) {
            const auto& remote_node = pair.second;
            
            // Skip in-process nodes - they are already handled by deliverInProcess
            if (local_node_ids.count(remote_node.node_id) > 0) {
                continue;
            }
            
            // Check if this node subscribed to the topic
            if (remote_node.subscriptions.find(key) != remote_node.subscriptions.end()) {
                target_nodes.push_back(remote_node);
            }
        }
    }
    
    // Send messages outside of the lock to avoid blocking other threads
    
    // Use lock-free transport for broadcast to inter-process nodes only
    // Note: In-process nodes are already handled by deliverInProcess, so we should
    // only send via shared memory if there are nodes outside current process
    bool has_interprocess_nodes = false;
    if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
        // Check if there are any nodes in shared memory that are NOT in our process
        for (const auto& shm_node_id : shm_node_ids) {
            if (local_node_ids.count(shm_node_id) == 0) {
                has_interprocess_nodes = true;
                break;
            }
        }
        
        // Only broadcast via shared memory if there are inter-process nodes
        if (has_interprocess_nodes) {
            shm_transport_v3_->broadcast(packet.data(), packet.size());
        }
    }
    
    // Send via UDP for non-local nodes
    for (const auto& remote_node : target_nodes) {
        // Determine if this node is local (in shared memory)
        bool use_shm = shm_node_ids.count(remote_node.node_id) > 0;
        
        // Remote or shared memory unavailable: use UDP (only if UDP is enabled)
        if (!use_shm && use_udp_ && udp_transport_ && !remote_node.address.empty() && remote_node.port > 0) {
            udp_transport_->send(packet.data(), packet.size(), 
                               remote_node.address, remote_node.port);
        }
    }
}

void NodeImpl::broadcastSubscription(const std::string& group,
                                    const std::string& topic,
                                    bool is_subscribe) {
    MessageType msg_type = is_subscribe ? MessageType::SUBSCRIBE : MessageType::UNSUBSCRIBE;
    auto packet = MessageBuilder::build(node_id_, group, topic, "", 
                                       getUdpPort(), msg_type);
    
    // Broadcast via lock-free shared memory to local nodes
    if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
        shm_transport_v3_->broadcast(packet.data(), packet.size());
    }
    
    // Broadcast via UDP if enabled
    if (!use_udp_ || !udp_transport_ || !udp_transport_->isInitialized()) {
        return;
    }
    
    // Copy remote nodes info to avoid holding lock during network I/O
    std::vector<std::pair<std::string, uint16_t>> remote_targets;
    bool need_full_scan = false;
    {
        std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
        
        // Send to all known remote nodes
        for (const auto& pair : remote_nodes_) {
            const auto& remote_node = pair.second;
            if (!remote_node.address.empty() && remote_node.port > 0) {
                remote_targets.emplace_back(remote_node.address, remote_node.port);
            }
        }
        
        // Check if we need full scan
        need_full_scan = remote_nodes_.empty();
    }
    
    // Send to known nodes (outside of lock)
    for (const auto& target : remote_targets) {
        udp_transport_->send(packet.data(), packet.size(), target.first, target.second);
    }
    
    // If no known nodes yet, do a full scan to announce ourselves
    if (need_full_scan) {
        const uint16_t my_port = getUdpPort();
        
        // Scan all ports to announce our subscription
        for (int port = PORT_BASE; port <= PORT_MAX; port++) {
            if (port != my_port) {
                udp_transport_->send(packet.data(), packet.size(), "127.0.0.1", port);
            }
        }
    }
}

void NodeImpl::handleSubscribe(const std::string& remote_node_id,
                               uint16_t remote_port,
                               const std::string& remote_addr,
                               const std::string& group,
                               const std::string& topic) {
    // Don't add ourselves to remote nodes list (self-subscription filter)
    if (remote_node_id == node_id_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    
    // Get or create remote node info
    auto& remote_node = remote_nodes_[remote_node_id];
    remote_node.node_id = remote_node_id;
    remote_node.port = remote_port;
    remote_node.address = remote_addr;
    
    // Add subscription
    SubscriptionKey key(group, topic);
    remote_node.subscriptions.insert(key);
}

void NodeImpl::handleUnsubscribe(const std::string& remote_node_id,
                                const std::string& group,
                                const std::string& topic) {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    
    auto it = remote_nodes_.find(remote_node_id);
    if (it == remote_nodes_.end()) {
        return;
    }
    
    // Remove subscription
    SubscriptionKey key(group, topic);
    it->second.subscriptions.erase(key);
    
    // Remove remote node if no subscriptions left
    if (it->second.subscriptions.empty()) {
        remote_nodes_.erase(it);
    }
}

void NodeImpl::queryExistingSubscriptions() {
    auto packet = MessageBuilder::build(node_id_, "", "", "", 
                                       getUdpPort(), MessageType::QUERY_SUBSCRIPTIONS);
    
    // Query UDP nodes
    if (!use_udp_ || !udp_transport_ || !udp_transport_->isInitialized()) {
        return;
    }
    
    // Scan all ports in the range to discover existing nodes
    // While this is 800 probes, QUERY messages are small and it's only done once at startup
    const uint16_t my_port = getUdpPort();
    
    // Scan all ports in range
    for (int port = PORT_BASE; port <= PORT_MAX; port++) {
        if (port != my_port) {
            udp_transport_->send(packet.data(), packet.size(), "127.0.0.1", port);
        }
    }
}

void NodeImpl::handleQuerySubscriptions(const std::string& remote_node_id,
                                       uint16_t remote_port,
                                       const std::string& remote_addr) {
    // Determine if this is a shared memory query
    bool is_shm_query = (remote_addr == "shm");
    
    // Copy subscriptions to avoid holding lock during network I/O
    std::vector<std::pair<std::string, std::vector<std::string>>> subscriptions_copy;
    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        
        for (const auto& sub_pair : subscriptions_) {
            const std::string& group = sub_pair.first;
            const auto& sub_info = sub_pair.second;
            
            std::vector<std::string> topics(sub_info.topics.begin(), sub_info.topics.end());
            subscriptions_copy.emplace_back(group, std::move(topics));
        }
    }
    
    // Reply with all our subscriptions (outside of lock)
    for (const auto& sub_pair : subscriptions_copy) {
        const std::string& group = sub_pair.first;
        const auto& topics = sub_pair.second;
        
        for (const auto& topic : topics) {
            // Send SUBSCRIPTION_REPLY message for each subscription
            auto packet = MessageBuilder::build(node_id_, group, topic, "", 
                                               getUdpPort(), MessageType::SUBSCRIPTION_REPLY);
            
            if (is_shm_query) {
                // Reply via lock-free shared memory
                if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
                    // For broadcast-based transport, just broadcast the reply
                    shm_transport_v3_->broadcast(packet.data(), packet.size());
                }
            } else if (use_udp_) {
                // Reply via UDP (only if UDP is enabled)
                if (udp_transport_ && !remote_addr.empty() && remote_port > 0) {
                    udp_transport_->send(packet.data(), packet.size(), remote_addr, remote_port);
                }
            }
        }
    }
}

void NodeImpl::handleSubscriptionReply(const std::string& remote_node_id,
                                       uint16_t remote_port,
                                       const std::string& remote_addr,
                                       const std::string& group,
                                       const std::string& topic) {
    // Don't add ourselves to remote nodes list (self-subscription filter)
    if (remote_node_id == node_id_) {
        return;
    }
    
    // This is essentially the same as handleSubscribe
    // The remote node is telling us about one of their subscriptions
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    
    auto& remote_node = remote_nodes_[remote_node_id];
    remote_node.node_id = remote_node_id;
    remote_node.port = remote_port;
    remote_node.address = remote_addr;
    
    // Add subscription
    SubscriptionKey key(group, topic);
    remote_node.subscriptions.insert(key);
}

void NodeImpl::registerNode() {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    node_registry_[node_id_] = shared_from_this();
}

void NodeImpl::unregisterNode() {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    node_registry_.erase(node_id_);
}

std::vector<std::shared_ptr<NodeImpl>> NodeImpl::getAllNodes() {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    
    std::vector<std::shared_ptr<NodeImpl>> result;
    result.reserve(node_registry_.size());
    
    // Clean up expired weak_ptr and collect valid nodes
    for (auto it = node_registry_.begin(); it != node_registry_.end(); ) {
        if (auto node = it->second.lock()) {
            result.push_back(node);
            ++it;
        } else {
            it = node_registry_.erase(it);
        }
    }
    
    return result;
}

void NodeImpl::enqueueMessage(const std::string& source_node_id,
                              const std::string& group,
                              const std::string& topic,
                              const uint8_t* payload,
                              size_t payload_len) {
    // Quick check if we're subscribed (avoid copying unnecessary data)
    {
        std::lock_guard<std::mutex> lock(subscriptions_mutex_);
        auto it = subscriptions_.find(group);
        if (it == subscriptions_.end() || 
            it->second.topics.find(topic) == it->second.topics.end()) {
            return; // Not subscribed
        }
    }
    
    // Prepare message
    PendingMessage msg;
    msg.source_node_id = source_node_id;
    msg.group = group;
    msg.topic = topic;
    msg.payload.assign(payload, payload + payload_len);
    
    // Use hash to determine which thread should process this group
    // Same group always goes to the same thread, ensuring order
    std::hash<std::string> hasher;
    size_t thread_id = hasher(group) % NUM_PROCESSING_THREADS;
    
    // Enqueue to specific thread's queue (with overflow protection)
    {
        std::lock_guard<std::mutex> lock(message_queue_mutexes_[thread_id]);
        auto& queue = message_queues_[thread_id];
        
        if (queue.size() >= MAX_QUEUE_SIZE) {
            // Queue full - drop oldest message to make room for new one
            queue.pop();  // Remove oldest
            dropped_messages_.fetch_add(1, std::memory_order_relaxed);
        }
        
        queue.push(std::move(msg));  // Add newest
    }
    
    // Notify the specific thread
    message_queue_cvs_[thread_id].notify_one();
}

void NodeImpl::messageProcessingThread(size_t thread_id) {
    static constexpr size_t MAX_BATCH_SIZE = 16;  // Process up to 16 messages per batch
    std::vector<PendingMessage> batch;
    batch.reserve(MAX_BATCH_SIZE);
    
    while (running_) {
        batch.clear();
        
        // Fetch a batch of messages (reduces lock contention)
        {
            std::unique_lock<std::mutex> lock(message_queue_mutexes_[thread_id]);
            message_queue_cvs_[thread_id].wait(lock, [this, thread_id] {
                return !running_ || !message_queues_[thread_id].empty();
            });
            
            if (!running_) {
                break;
            }
            
            // Extract multiple messages at once
            auto& queue = message_queues_[thread_id];
            while (!queue.empty() && batch.size() < MAX_BATCH_SIZE) {
                batch.push_back(std::move(queue.front()));
                queue.pop();
            }
        }
        
        // Process all messages in batch (outside of queue lock)
        for (auto& msg : batch) {
            // Get callback (subscription already verified in enqueueMessage)
            Callback callback;
            {
                std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                auto it = subscriptions_.find(msg.group);
                if (it != subscriptions_.end()) {
                    callback = it->second.callback;
                }
            }
            
            // Invoke callback outside of lock
            if (callback) {
                try {
                    callback(msg.group, msg.topic, msg.payload.data(), msg.payload.size());
                } catch (...) {
                    // Ignore callback exceptions to prevent thread crash
                }
            }
        }
    }
}

NodeImpl::QueueStats NodeImpl::getQueueStats() const {
    QueueStats stats = {};
    
    // Get current queue depth for each thread
    for (size_t i = 0; i < NUM_PROCESSING_THREADS; ++i) {
        std::lock_guard<std::mutex> lock(message_queue_mutexes_[i]);
        stats.queue_depth[i] = message_queues_[i].size();
    }
    
    // Get total dropped messages
    stats.total_dropped = dropped_messages_.load(std::memory_order_relaxed);
    
    return stats;
}

// Factory functions
std::shared_ptr<Node> createNode(const std::string& node_id, TransportMode mode) {
    // Always enable UDP with auto-selected port (0 = auto-select)
    // The framework will automatically choose between in-process and inter-process communication
    auto node = std::make_shared<NodeImpl>(node_id, false, 0, mode);
    std::static_pointer_cast<NodeImpl>(node)->initialize(0);
    return node;
}

// Default instance (singleton)
static std::weak_ptr<Node> g_default_node;
static std::mutex g_default_mutex;

std::shared_ptr<Node> communicationInterface() {
    std::lock_guard<std::mutex> lock(g_default_mutex);
    
    auto node = g_default_node.lock();
    if (!node) {
        node = createNode("default_node");
        g_default_node = node;
    }
    
    return node;
}

} // namespace librpc
