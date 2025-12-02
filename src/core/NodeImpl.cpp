#include "nexus/core/NodeImpl.h"
#include "nexus/transport/UdpTransport.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/registry/GlobalRegistry.h"
#include "nexus/core/Config.h"
#include "nexus/utils/Logger.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iostream>
#include <cstring>

namespace Nexus {
namespace rpc {

// Port range constants for node discovery
static constexpr uint16_t PORT_BASE = 47200;
static constexpr uint16_t PORT_MAX = 47999;
static constexpr uint16_t PORT_COUNT = PORT_MAX - PORT_BASE + 1;  // 800 ports

// Note: Static members replaced by Nexus::rpc::GlobalRegistry
// - node_registry_ -> GlobalRegistry::instance().registerNode()
// - global_service_registry_ -> GlobalRegistry::instance().registerService()

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
    
    // Stop UDP heartbeat thread
    stopUdpHeartbeat();
    
    // Stop cleanup thread
    cleanup_running_ = false;
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
    
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
    
    // Verify config matches compile-time constants
    auto& config = Nexus::rpc::Config::instance();
    if (config.node.num_processing_threads != NUM_PROCESSING_THREADS) {
        NEXUS_WARN("IMPL") << "Config thread count (" << config.node.num_processing_threads 
            << ") doesn't match compile-time NUM_PROCESSING_THREADS (" << NUM_PROCESSING_THREADS << ")";
    }
    
    // Start message processing threads
    for (size_t i = 0; i < NUM_PROCESSING_THREADS; ++i) {
        processing_threads_.emplace_back(&NodeImpl::messageProcessingThread, this, i);
    }
    
    // Initialize lock-free shared memory transport
    if (transport_mode_ == TransportMode::AUTO || transport_mode_ == TransportMode::LOCK_FREE_SHM) {
        shm_transport_v3_ = std::make_unique<SharedMemoryTransportV3>();
        if (!shm_transport_v3_->initialize(node_id_)) {
            NEXUS_LOG_ERROR("IMPL", "Lock-free shared memory initialization failed");
            shm_transport_v3_.reset();
        } else {
            NEXUS_LOG_INFO("IMPL", "Using lock-free shared memory transport (V3 - Dynamic)");
            
            // Set NodeImpl reference for heartbeat timeout notifications
            shm_transport_v3_->setNodeImpl(this);
            
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
                    case MessageType::DATA:
                        handleMessage(source_node, group, topic, 
                                    packet->getPayload(), packet->payload_len);
                        break;
                        
                    case MessageType::SERVICE_REGISTER:
                    case MessageType::SERVICE_UNREGISTER:
                        handleServiceMessage(source_node, group, topic,
                                           packet->getPayload(), packet->payload_len,
                                           msg_type == MessageType::SERVICE_REGISTER);
                        break;
                        
                    case MessageType::NODE_JOIN:
                        handleNodeEvent(source_node, true);
                        break;
                        
                    case MessageType::NODE_LEAVE:
                        handleNodeEvent(source_node, false);
                        break;
                        
                    case MessageType::HEARTBEAT:
                        // Shared memory doesn't need heartbeat (managed by OS)
                        break;
                }
            });
            shm_transport_v3_->startReceiving();
            
            // Shared memory: No need for QUERY_SUBSCRIPTIONS
            // Nodes are automatically discovered via SharedMemoryRegistry
            // Subscriptions are synchronized via SUBSCRIBE broadcast messages
            // This eliminates ~70% of startup messages and speeds up initialization
            
            // Query existing services from other nodes (cross-process service discovery)
            queryRemoteServices();
            
            // Broadcast NODE_JOIN event to other processes (after transport is ready)
            broadcastNodeEvent(true);
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
                case MessageType::DATA:
                    handleMessage(source_node, group, topic, 
                                packet->getPayload(), packet->payload_len);
                    break;
                
                case MessageType::SERVICE_REGISTER:
                case MessageType::SERVICE_UNREGISTER:
                    handleServiceMessage(source_node, group, topic,
                                       packet->getPayload(), packet->payload_len,
                                       msg_type == MessageType::SERVICE_REGISTER);
                    break;
                    
                case MessageType::NODE_JOIN:
                    handleNodeEvent(source_node, true);
                    break;
                    
                case MessageType::NODE_LEAVE:
                    handleNodeEvent(source_node, false);
                    break;
                    
                case MessageType::HEARTBEAT:
                    handleUdpHeartbeat(source_node, from_addr, sender_port);
                    break;
                    
                case MessageType::QUERY_SUBSCRIPTIONS:
                    // Reply with all our services (UDP service discovery)
                    handleQuerySubscriptions(source_node, sender_port, from_addr);
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
        
        // Start UDP heartbeat thread
        startUdpHeartbeat();
    }
    
    // Start background cleanup thread (runs every 5 minutes)
    cleanup_running_ = true;
    cleanup_thread_ = std::thread(&NodeImpl::cleanupThreadFunc, this);
}

Node::Error NodeImpl::publish(const Property& msg_group, 
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
    
    // Auto-register services (service discovery)
    for (const auto& topic : topics) {
        if (!topic.empty()) {
            // Register for shared memory transport (if available)
            if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
                ServiceDescriptor svc;
                svc.node_id = node_id_;
                svc.group = msg_group;
                svc.topic = topic;
                svc.type = ServiceType::NORMAL_MESSAGE;
                svc.channel_name = "";  // Not a large data channel
                svc.transport = TransportType::SHARED_MEMORY;
                svc.udp_address = "";
                
                registerService(svc);
            }
            
            // Register for UDP transport (if enabled)
            if (use_udp_ && udp_transport_ && udp_transport_->isInitialized()) {
                ServiceDescriptor svc;
                svc.node_id = node_id_;
                svc.group = msg_group;
                svc.topic = topic;
                svc.type = ServiceType::NORMAL_MESSAGE;
                svc.channel_name = "";
                svc.transport = TransportType::UDP;
                svc.udp_address = "0.0.0.0:" + std::to_string(getUdpPort());
                
                registerService(svc);
            }
        }
    }
    
    // No need to broadcast SUBSCRIBE - service registration handles discovery
    
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
    
    // Auto-unregister services for collected topics (outside of lock)
    for (const auto& topic : topics_to_broadcast) {
        // Auto-unregister services (both SHM and UDP)
        if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
            ServiceDescriptor svc;
            svc.node_id = node_id_;
            svc.group = msg_group;
            svc.topic = topic;
            svc.type = ServiceType::NORMAL_MESSAGE;
            svc.channel_name = "";
            svc.transport = TransportType::SHARED_MEMORY;
            svc.udp_address = "";
            
            unregisterService(svc);
        }
        
        if (use_udp_ && udp_transport_ && udp_transport_->isInitialized()) {
            ServiceDescriptor svc;
            svc.node_id = node_id_;
            svc.group = msg_group;
            svc.topic = topic;
            svc.type = ServiceType::NORMAL_MESSAGE;
            svc.channel_name = "";
            svc.transport = TransportType::UDP;
            svc.udp_address = "0.0.0.0:" + std::to_string(getUdpPort());
            
            unregisterService(svc);
        }
    }
    
    return Error::NO_ERROR;
}

Node::Error NodeImpl::sendLargeData(const std::string& msg_group,
                                   const std::string& channel_name,
                                   const std::string& topic,
                                   const uint8_t* data,
                                   size_t size) {
    // Validate parameters
    if (msg_group.empty() || channel_name.empty() || topic.empty() || !data || size == 0) {
        return Error::INVALID_ARG;
    }
    
    if (!running_) {
        return Error::NOT_INITIALIZED;
    }
    
    // Auto-register large data service (first send only)
    std::string capability = msg_group + "/" + channel_name + "/" + topic;
    {
        std::lock_guard<std::mutex> lock(capabilities_mutex_);
        if (capabilities_.find(capability) == capabilities_.end()) {
            // Register for shared memory (large data only supports SHM)
            if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
                ServiceDescriptor svc;
                svc.node_id = node_id_;
                svc.group = msg_group;
                svc.topic = topic;
                svc.type = ServiceType::LARGE_DATA;
                svc.channel_name = channel_name;
                svc.transport = TransportType::SHARED_MEMORY;
                svc.udp_address = "";
                
                registerService(svc);
                capabilities_.insert(capability);
            }
        }
    }
    
    // Get or create the large data channel
    auto channel = getLargeDataChannel(channel_name);
    if (!channel) {
        return Error::UNEXPECTED_ERROR;
    }
    
    // Check if there's enough space
    size_t required = sizeof(LargeDataHeader) + size;
    if (!channel->canWrite(required)) {
        return Error::TIMEOUT;  // Buffer full
    }
    
    // Write data to the channel
    int64_t seq = channel->write(topic, data, size);
    if (seq < 0) {
        return Error::UNEXPECTED_ERROR;
    }
    
    // Send notification via V3 message queue (only 128 bytes)
    LargeDataNotification notif{};
    notif.sequence = static_cast<uint64_t>(seq);
    notif.size = static_cast<uint32_t>(size);
    notif.reserved1 = 0;
    
    // Copy channel name and topic (with bounds checking)
    strncpy(notif.channel_name, channel_name.c_str(), sizeof(notif.channel_name) - 1);
    notif.channel_name[sizeof(notif.channel_name) - 1] = '\0';
    
    strncpy(notif.topic, topic.c_str(), sizeof(notif.topic) - 1);
    notif.topic[sizeof(notif.topic) - 1] = '\0';
    
    memset(notif.reserved2, 0, sizeof(notif.reserved2));
    
    // Publish notification as a string (convert struct to string)
    std::string notif_str(reinterpret_cast<const char*>(&notif), sizeof(notif));
    
    // Use user-specified msg_group and topic for the notification
    Error err = publish(msg_group, topic, notif_str);
    if (err != NO_ERROR) {
        return err;
    }
    
    return NO_ERROR;
}

std::shared_ptr<LargeDataChannel> NodeImpl::getLargeDataChannel(const std::string& channel_name) {
    if (channel_name.empty()) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(large_channels_mutex_);
    
    // Check if channel already exists
    auto it = large_channels_.find(channel_name);
    if (it != large_channels_.end()) {
        return it->second;
    }
    
    // Create new channel with default optimized configuration
    // Note: Using MAP_NORESERVE means 64MB is reserved but not allocated until used
    // Actual memory usage depends on real data written
    LargeDataChannel::Config config;
    config.use_mmap_noreserve = true;       // Memory-efficient: only allocates when written
    config.buffer_size = 64 * 1024 * 1024;  // 64MB virtual address space
    config.max_block_size = 8 * 1024 * 1024; // 8MB max block
    
    // Create channel (mmap with MAP_NORESERVE, actual pages allocated on write)
    auto channel = LargeDataChannel::create(channel_name, config);
    if (channel) {
        large_channels_[channel_name] = channel;
        
        NEXUS_INFO("LargeData") << "Created large data channel: " << channel_name
            << ", size: " << (config.buffer_size / 1024 / 1024) << " MB"
            << ", MAP_NORESERVE: yes (lazy allocation)";
    }
    
    return channel;
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
    // ✅ Optimization 2: Query services first, then build node sets only if needed
    // This avoids unnecessary getAllNodes() calls when there are no inter-process subscribers
    auto& registry = Nexus::rpc::GlobalRegistry::instance();
    auto services = registry.findServices(group);
    
    // Quick check: if no services for this group, return early
    if (services.empty()) {
        return;
    }
    
    // Build a set of local node IDs (in-process nodes) - only if we have services
    std::set<std::string> local_node_ids;
    {
        auto nodes = registry.getAllNodes();
        for (const auto& node : nodes) {
            if (node) {
                local_node_ids.insert(node->getNodeId());
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
    
    // ✅ Optimization 3: Reserve capacity to avoid reallocation
    // Separate subscribers by transport type
    std::vector<std::string> shm_subscribers;
    std::vector<std::pair<std::string, uint16_t>> udp_subscribers;  // (address, port)
    std::set<std::string> delivered_nodes;  // Avoid duplicate delivery
    
    shm_subscribers.reserve(8);  // Reserve space for typical case (避免多次realloc)
    udp_subscribers.reserve(8);
    
    for (const auto& svc : services) {
        // Skip services that don't match this topic
        if (svc.topic != topic) {
            continue;
        }
        
        // Skip ourselves
        if (svc.node_id == node_id_) {
            continue;
        }
        
        // Skip in-process nodes - they are already handled by deliverInProcess
        if (local_node_ids.count(svc.node_id) > 0) {
            continue;
        }
        
        // Skip if already delivered to this node (avoid duplicate via different transports)
        if (delivered_nodes.count(svc.node_id) > 0) {
            continue;
        }
        
        // Choose transport based on service registration
        if (svc.transport == TransportType::SHARED_MEMORY) {
            // Verify node is actually in shared memory
            if (shm_node_ids.count(svc.node_id) > 0) {
                shm_subscribers.push_back(svc.node_id);
                delivered_nodes.insert(svc.node_id);
            }
        } else if (svc.transport == TransportType::UDP) {
            // ✅ Optimization 4: Parse UDP address with validation (avoid exceptions in hot path)
            if (!svc.udp_address.empty()) {
                const size_t colon_pos = svc.udp_address.find(':');
                if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < svc.udp_address.length() - 1) {
                    const std::string& ip_str = svc.udp_address.substr(0, colon_pos);
                    const std::string& ip = (ip_str == "0.0.0.0") ? "127.0.0.1" : ip_str;
                    
                    try {
                        uint16_t port = static_cast<uint16_t>(std::stoi(svc.udp_address.substr(colon_pos + 1)));
                        udp_subscribers.emplace_back(ip, port);
                        delivered_nodes.insert(svc.node_id);
                    } catch (const std::exception&) {
                        // Skip invalid port number
                    }
                }
            }
        }
    }
    
    // ✅ Optimized: Point-to-point send to each subscriber
    // 1. Send via shared memory
    if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
        for (const auto& subscriber_id : shm_subscribers) {
            shm_transport_v3_->send(subscriber_id, packet.data(), packet.size());
        }
    }
    
    // 2. Send via UDP
    if (use_udp_ && udp_transport_ && udp_transport_->isInitialized()) {
        for (const auto& addr_port : udp_subscribers) {
            udp_transport_->send(packet.data(), packet.size(), addr_port.first, addr_port.second);
        }
    }
}

// SUBSCRIBE/UNSUBSCRIBE handlers (deprecated - kept for interface compatibility)
// Service discovery now uses SERVICE_REGISTER/UNREGISTER mechanism

void NodeImpl::handleSubscribe(const std::string&,
                               uint16_t,
                               const std::string&,
                               const std::string&,
                               const std::string&) {
    // No-op: replaced by SERVICE_REGISTER
}

void NodeImpl::handleUnsubscribe(const std::string&,
                                const std::string&,
                                const std::string&) {
    // No-op: replaced by SERVICE_UNREGISTER
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

void NodeImpl::registerNode() {
    auto& registry = Nexus::rpc::GlobalRegistry::instance();
    bool is_new_node = (registry.findNode(node_id_) == nullptr);
    
    registry.registerNode(node_id_, shared_from_this());
    
    // Notify other nodes about NODE_JOINED event (only if this is a new node)
    // Note: Cross-process notification is deferred until after transport initialization
    // (see initialize() method where broadcastNodeEvent(true) is called after SHM setup)
    if (is_new_node) {
        // In-process notification only
        auto nodes = getAllNodes();
        for (auto& node : nodes) {
            if (node && node->getNodeId() != node_id_) {
                // Trigger NODE_JOINED callback
                node->notifyNodeEvent(ServiceEvent::NODE_JOINED, node_id_);
            }
        }
    }
}

void NodeImpl::unregisterNode() {
    // Notify other nodes about NODE_LEFT event before removal
    {
        // In-process notification
        auto nodes = getAllNodes();
        for (auto& node : nodes) {
            if (node && node->getNodeId() != node_id_) {
                // Trigger NODE_LEFT callback
                node->notifyNodeEvent(ServiceEvent::NODE_LEFT, node_id_);
            }
        }
        
        // Cross-process notification (via shared memory)
        broadcastNodeEvent(false);
    }
    
    // Remove from registry and clean up all services
    // GlobalRegistry::unregisterNode() will automatically remove all services
    // registered by this node to prevent "zombie services"
    auto& registry = Nexus::rpc::GlobalRegistry::instance();
    registry.unregisterNode(node_id_);
}

std::vector<std::shared_ptr<NodeImpl>> NodeImpl::getAllNodes() {
    return Nexus::rpc::GlobalRegistry::instance().getAllNodes();
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
    
    // 负载均衡策略：同一个 group:topic 必须映射到同一个线程，保证消息顺序
    // 使用哈希确保同topic消息的顺序性，同时实现不同topic的负载均衡
    std::hash<std::string> hasher;
    std::string routing_key = group + ":" + topic;
    size_t thread_id = hasher(routing_key) % NUM_PROCESSING_THREADS;
    
    // Get max queue size from config
    auto& config = Nexus::rpc::Config::instance();
    const size_t max_queue_size = config.node.max_queue_size;
    
    // Enqueue to specific thread's queue (with overflow protection)
    {
        std::lock_guard<std::mutex> lock(message_queue_mutexes_[thread_id]);
        auto& queue = message_queues_[thread_id];
        
        if (queue.size() >= max_queue_size) {
            // Queue full - apply overflow policy
            bool message_dropped = false;
            PendingMessage* dropped_msg = nullptr;
            
            switch (overflow_policy_) {
                case QueueOverflowPolicy::DROP_OLDEST:
                    // Drop oldest message to make room for new one
                    dropped_msg = &queue.front();
                    queue.pop();
                    queue.push(std::move(msg));
                    message_dropped = true;
                    break;
                    
                case QueueOverflowPolicy::DROP_NEWEST:
                    // Drop the new message
                    dropped_msg = &msg;
                    message_dropped = true;
                    // Don't add to queue
                    break;
                    
                case QueueOverflowPolicy::BLOCK:
                    // This should not happen in practice with current design
                    // But if it does, drop oldest to prevent deadlock
                    queue.pop();
                    queue.push(std::move(msg));
                    break;
            }
            
            if (message_dropped) {
                size_t total_dropped = dropped_messages_.fetch_add(1, std::memory_order_relaxed) + 1;
                
                // Call overflow callback if set
                {
                    std::lock_guard<std::mutex> cb_lock(overflow_callback_mutex_);
                    if (overflow_callback_) {
                        try {
                            overflow_callback_(dropped_msg->group, dropped_msg->topic, total_dropped);
                        } catch (...) {
                            // Ignore callback exceptions
                        }
                    }
                }
            }
        } else {
            queue.push(std::move(msg));  // Add to queue
        }
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
        for (const auto& msg : batch) {
            // Get callback (subscription already verified in enqueueMessage)
            Callback callback;
            {
                std::lock_guard<std::mutex> lock(subscriptions_mutex_);
                const auto it = subscriptions_.find(msg.group);
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

void NodeImpl::setQueueOverflowPolicy(QueueOverflowPolicy policy) {
    overflow_policy_ = policy;
}

void NodeImpl::setQueueOverflowCallback(QueueOverflowCallback callback) {
    std::lock_guard<std::mutex> lock(overflow_callback_mutex_);
    overflow_callback_ = callback;
}

size_t NodeImpl::cleanupOrphanedChannels() {
    // Cleanup LargeDataChannel orphaned shared memory
    size_t cleaned = LargeDataChannel::cleanupOrphanedChannels(60);
    
    // Also cleanup SharedMemoryTransportV3 orphaned memory if needed
    if (shm_transport_v3_) {
        // V3 has its own cleanup in destructor, but we can force it here
        // For now, just rely on LargeDataChannel cleanup
    }
    
    return cleaned;
}

void NodeImpl::cleanupThreadFunc() {
    NEXUS_LOG_INFO("IMPL", "Background cleanup thread started for node " + node_id_);
    
    while (cleanup_running_) {
        // Sleep for 5 minutes
        for (int i = 0; i < 300 && cleanup_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!cleanup_running_) {
            break;
        }
        
        // Perform cleanup
        size_t cleaned = cleanupOrphanedChannels();
        if (cleaned > 0) {
            NEXUS_INFO("IMPL") << "Background cleanup: removed " << cleaned << " orphaned channel(s)";
        }
    }
    
    NEXUS_LOG_INFO("IMPL", "Background cleanup thread stopped for node " + node_id_);
}

// ==================== Service Discovery Implementation ====================

void NodeImpl::queryRemoteServices() {
    //  Query existing services from other processes via shared memory
    // Strategy: Broadcast a special request, and existing nodes will reply with their services
    
    if (!shm_transport_v3_ || !shm_transport_v3_->isInitialized()) {
        return;
    }
    
    // Send empty SERVICE_REGISTER message as a query (payload_len = 0 means "query")
    // Other nodes will respond by re-broadcasting their services
    std::vector<uint8_t> query_packet = MessageBuilder::build(
        node_id_, "", "", nullptr, 0, 0, MessageType::SERVICE_REGISTER);
    
    shm_transport_v3_->broadcast(query_packet.data(), query_packet.size());
    
    // Give other nodes time to respond
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void NodeImpl::registerService(const ServiceDescriptor& svc) {
    // Add to global registry
    Nexus::rpc::GlobalRegistry::instance().registerService(svc.group, svc);
    
    // Notify in-process nodes
    auto nodes = getAllNodes();
    for (auto& node : nodes) {
        if (node && node->getNodeId() != node_id_) {
            node->handleServiceUpdate(node_id_, svc, true);
        }
    }
    
    // Broadcast to remote nodes (inter-process via shared memory)
    broadcastServiceUpdate(svc, true);
}

void NodeImpl::unregisterService(const ServiceDescriptor& svc) {
    // Remove from global registry
    Nexus::rpc::GlobalRegistry::instance().unregisterService(svc.group, svc);
    
    // Notify in-process nodes
    auto nodes = getAllNodes();
    for (auto& node : nodes) {
        if (node && node->getNodeId() != node_id_) {
            node->handleServiceUpdate(node_id_, svc, false);
        }
    }
    
    // Broadcast to remote nodes (inter-process via shared memory)
    broadcastServiceUpdate(svc, false);
}

void NodeImpl::broadcastNodeEvent(bool is_joined) {
    // Broadcast NODE_JOIN or NODE_LEAVE event to other nodes via shared memory
    if (!shm_transport_v3_ || !shm_transport_v3_->isInitialized()) {
        return;
    }
    
    MessageType msg_type = is_joined ? MessageType::NODE_JOIN : MessageType::NODE_LEAVE;
    
    // Node event message has empty payload (node_id is in message header)
    auto packet = MessageBuilder::build(node_id_, "", "", nullptr, 0, getUdpPort(), msg_type);
    
    shm_transport_v3_->broadcast(packet.data(), packet.size());
}

void NodeImpl::handleNodeEvent(const std::string& from_node, bool is_joined) {
    if (from_node == node_id_) {
        return;  // Ignore self events
    }
    
    ServiceEvent event = is_joined ? ServiceEvent::NODE_JOINED : ServiceEvent::NODE_LEFT;
    
    // Notify all listeners in this node
    notifyNodeEvent(event, from_node);
    
    if (is_joined) {
        // ✅ No need to query new node's services
        // The new node already sent queryRemoteServices() to discover our services
        // And future service changes will be auto-broadcasted via subscribe/unsubscribe
        // This eliminates redundant bidirectional queries
    } else {
        // Node left: Clean up its services from global registry
        // For cross-process nodes, we need to manually clean up services
        // because GlobalRegistry doesn't know about remote node disconnections
        auto& registry = Nexus::rpc::GlobalRegistry::instance();
        auto all_services = registry.findServices();
        
        // Remove all services registered by the departed node
        for (const auto& svc : all_services) {
            if (svc.node_id == from_node) {
                registry.unregisterService(svc.group, svc);
            }
        }
    }
}

void NodeImpl::broadcastServiceUpdate(const ServiceDescriptor& svc, bool is_add) {
    // Serialize service descriptor to payload
    // Format: type(1byte) + transport(1byte) + channel_name_len(1byte) + udp_addr_len(2bytes) + channel_name + udp_address
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(svc.type));
    payload.push_back(static_cast<uint8_t>(svc.transport));
    
    uint8_t channel_len = static_cast<uint8_t>(svc.channel_name.size());
    payload.push_back(channel_len);
    
    uint16_t udp_addr_len = static_cast<uint16_t>(svc.udp_address.size());
    payload.push_back(static_cast<uint8_t>(udp_addr_len & 0xFF));
    payload.push_back(static_cast<uint8_t>((udp_addr_len >> 8) & 0xFF));
    
    if (channel_len > 0) {
        payload.insert(payload.end(), svc.channel_name.begin(), svc.channel_name.end());
    }
    if (udp_addr_len > 0) {
        payload.insert(payload.end(), svc.udp_address.begin(), svc.udp_address.end());
    }
    
    MessageType msg_type = is_add ? MessageType::SERVICE_REGISTER : MessageType::SERVICE_UNREGISTER;
    auto packet = MessageBuilder::build(node_id_, svc.group, svc.topic,
                                       payload.data(), payload.size(),
                                       getUdpPort(), msg_type);
    
    // Broadcast via shared memory to local inter-process nodes
    if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
        shm_transport_v3_->broadcast(packet.data(), packet.size());
    }
    
    // Broadcast via UDP to remote nodes (if this is a UDP service or for discovery)
    if (use_udp_ && udp_transport_ && udp_transport_->isInitialized()) {
        // Get all known UDP endpoints
        auto& registry = Nexus::rpc::GlobalRegistry::instance();
        auto all_services = registry.findServices();
        
        std::set<std::string> udp_endpoints;
        for (const auto& remote_svc : all_services) {
            if (remote_svc.transport == TransportType::UDP && 
                remote_svc.node_id != node_id_ && 
                !remote_svc.udp_address.empty()) {
                udp_endpoints.insert(remote_svc.udp_address);
            }
        }
        
        // Send to each endpoint
        for (const auto& endpoint : udp_endpoints) {
            const size_t colon_pos = endpoint.find(':');
            if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == endpoint.length() - 1) {
                continue;  // Invalid endpoint format
            }
            
            const std::string ip_str = endpoint.substr(0, colon_pos);
            const std::string& ip = (ip_str == "0.0.0.0") ? "127.0.0.1" : ip_str;
            
            try {
                const uint16_t port = static_cast<uint16_t>(std::stoi(endpoint.substr(colon_pos + 1)));
                udp_transport_->send(packet.data(), packet.size(), ip, port);
            } catch (const std::exception&) {
                // Skip invalid port number
                continue;
            }
        }
    }
}

void NodeImpl::handleServiceUpdate(const std::string& from_node,
                                  const ServiceDescriptor& svc,
                                  bool is_add) {
    // Invoke callback if set
    ServiceDiscoveryCallback callback;
    {
        std::lock_guard<std::mutex> lock(service_callback_mutex_);
        callback = service_discovery_callback_;
    }
    
    if (callback) {
        ServiceEvent event = is_add ? ServiceEvent::SERVICE_ADDED : ServiceEvent::SERVICE_REMOVED;
        try {
            callback(event, svc);
        } catch (...) {
            // Ignore exceptions from user callback
        }
    }
}

void NodeImpl::notifyNodeEvent(ServiceEvent event, const std::string& node_id) {
    // Invoke callback if set
    ServiceDiscoveryCallback callback;
    {
        std::lock_guard<std::mutex> lock(service_callback_mutex_);
        callback = service_discovery_callback_;
    }
    
    if (callback) {
        // Create a dummy service descriptor for node events
        ServiceDescriptor svc;
        svc.node_id = node_id;
        svc.type = ServiceType::ALL;  // Node event, not specific to service type
        
        try {
            callback(event, svc);
        } catch (...) {
            // Ignore exceptions from user callback
        }
    }
}

void NodeImpl::handleServiceMessage(const std::string& from_node,
                                   const std::string& group,
                                   const std::string& topic,
                                   const uint8_t* payload,
                                   size_t payload_len,
                                   bool is_register) {
    // Skip our own messages
    if (from_node == node_id_) {
        return;
    }
    
    // Special case: empty payload means "query all services" request
    if (is_register && payload_len == 0) {
        // Another node is asking for our services, send them point-to-point
        auto& registry = Nexus::rpc::GlobalRegistry::instance();
        auto services = registry.findServices();
        
        // Reply with each of our services (point-to-point send to requesting node)
        for (const auto& svc : services) {
            if (svc.node_id == node_id_ && svc.transport == TransportType::SHARED_MEMORY) {
                // Serialize service descriptor to payload
                std::vector<uint8_t> svc_payload;
                svc_payload.push_back(static_cast<uint8_t>(svc.type));
                svc_payload.push_back(static_cast<uint8_t>(svc.transport));
                
                uint8_t channel_len = static_cast<uint8_t>(svc.channel_name.size());
                svc_payload.push_back(channel_len);
                
                uint16_t udp_addr_len = static_cast<uint16_t>(svc.udp_address.size());
                svc_payload.push_back(static_cast<uint8_t>(udp_addr_len & 0xFF));
                svc_payload.push_back(static_cast<uint8_t>((udp_addr_len >> 8) & 0xFF));
                
                if (channel_len > 0) {
                    svc_payload.insert(svc_payload.end(), svc.channel_name.begin(), svc.channel_name.end());
                }
                if (udp_addr_len > 0) {
                    svc_payload.insert(svc_payload.end(), svc.udp_address.begin(), svc.udp_address.end());
                }
                
                // Build SERVICE_REGISTER packet
                auto packet = MessageBuilder::build(node_id_, svc.group, svc.topic,
                                                   svc_payload.data(), svc_payload.size(),
                                                   getUdpPort(), MessageType::SERVICE_REGISTER);
                
                // Send directly to the requesting node (point-to-point via shared memory)
                if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
                    shm_transport_v3_->send(from_node, packet.data(), packet.size());
                }
            }
        }
        return;
    }
    
    // Deserialize service descriptor from payload
    // Format: type(1byte) + transport(1byte) + channel_name_len(1byte) + udp_addr_len(2bytes) + channel_name + udp_address
    if (payload_len < 5) {
        return;  // Invalid payload
    }
    
    ServiceDescriptor svc;
    svc.node_id = from_node;
    svc.group = group;
    svc.topic = topic;
    svc.type = static_cast<ServiceType>(payload[0]);
    svc.transport = static_cast<TransportType>(payload[1]);
    
    uint8_t channel_len = payload[2];
    uint16_t udp_addr_len = static_cast<uint16_t>(payload[3]) | 
                           (static_cast<uint16_t>(payload[4]) << 8);
    
    if (payload_len < static_cast<size_t>(5 + channel_len + udp_addr_len)) {
        return;  // Invalid payload
    }
    
    if (channel_len > 0) {
        svc.channel_name = std::string(reinterpret_cast<const char*>(payload + 5), channel_len);
    }
    if (udp_addr_len > 0) {
        svc.udp_address = std::string(reinterpret_cast<const char*>(payload + 5 + channel_len), udp_addr_len);
    }
    
    auto& registry = Nexus::rpc::GlobalRegistry::instance();
    
    if (is_register) {
        // Add to global registry (cross-process service)
        registry.registerService(svc.group, svc);
        
        // Trigger SERVICE_ADDED callback
        handleServiceUpdate(from_node, svc, true);
    } else {
        // Remove from global registry
        registry.unregisterService(svc.group, svc);
        
        // Trigger SERVICE_REMOVED callback
        handleServiceUpdate(from_node, svc, false);
    }
}

std::vector<ServiceDescriptor> NodeImpl::discoverServices(
    const std::string& group,
    ServiceType type) {
    
    std::vector<ServiceDescriptor> result;
    
    // Get all services from GlobalRegistry
    auto all_services = Nexus::rpc::GlobalRegistry::instance().findServices(group);
    
    for (const auto& svc : all_services) {
        // Filter by type
        if (type != ServiceType::ALL && svc.type != type) {
            continue;
        }
        
        result.push_back(svc);
    }
    
    return result;
}

std::vector<std::string> NodeImpl::findNodesByCapability(
    const std::string& capability) {
    
    std::vector<std::string> result;
    
    // Get all services from GlobalRegistry
    auto all_services = Nexus::rpc::GlobalRegistry::instance().findServices();
    
    for (const auto& svc : all_services) {
        if (svc.getCapability() == capability) {
            // Check if already in result
            if (std::find(result.begin(), result.end(), svc.node_id) == result.end()) {
                result.push_back(svc.node_id);
            }
        }
    }
    
    return result;
}

std::vector<ServiceDescriptor> NodeImpl::findLargeDataChannels(
    const std::string& group) {
    
    return discoverServices(group, ServiceType::LARGE_DATA);
}

void NodeImpl::setServiceDiscoveryCallback(ServiceDiscoveryCallback callback) {
    std::lock_guard<std::mutex> lock(service_callback_mutex_);
    service_discovery_callback_ = callback;
}

// ==================== End Service Discovery ====================

// ==================== UDP Heartbeat Implementation ====================

void NodeImpl::startUdpHeartbeat() {
    if (!use_udp_ || !udp_transport_ || !udp_transport_->isInitialized()) {
        return;
    }
    
    udp_heartbeat_running_ = true;
    udp_heartbeat_thread_ = std::thread(&NodeImpl::udpHeartbeatThread, this);
}

void NodeImpl::stopUdpHeartbeat() {
    udp_heartbeat_running_ = false;
    if (udp_heartbeat_thread_.joinable()) {
        udp_heartbeat_thread_.join();
    }
}

void NodeImpl::udpHeartbeatThread() {
    NEXUS_LOG_INFO("IMPL", "UDP heartbeat thread started for node " + node_id_);
    
    while (udp_heartbeat_running_) {
        // Send heartbeat to all known UDP nodes
        sendUdpHeartbeat();
        
        // Check for timeouts and clean up dead nodes
        checkUdpTimeouts();
        
        // Sleep for heartbeat interval
        for (int i = 0; i < UDP_HEARTBEAT_INTERVAL_MS / 100 && udp_heartbeat_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    NEXUS_LOG_INFO("IMPL", "UDP heartbeat thread stopped for node " + node_id_);
}

void NodeImpl::sendUdpHeartbeat() {
    if (!use_udp_ || !udp_transport_ || !udp_transport_->isInitialized()) {
        return;
    }
    
    // Build heartbeat packet (empty payload)
    auto packet = MessageBuilder::build(node_id_, "", "", nullptr, 0, 
                                       getUdpPort(), MessageType::HEARTBEAT);
    
    // Get all UDP services from GlobalRegistry
    auto& registry = Nexus::rpc::GlobalRegistry::instance();
    auto all_services = registry.findServices();
    
    // Collect unique UDP endpoints
    std::set<std::string> udp_endpoints;  // Format: "ip:port"
    for (const auto& svc : all_services) {
        if (svc.transport == TransportType::UDP && 
            svc.node_id != node_id_ && 
            !svc.udp_address.empty()) {
            udp_endpoints.insert(svc.udp_address);
        }
    }
    
    // Send heartbeat to each endpoint
    for (const auto& endpoint : udp_endpoints) {
        const size_t colon_pos = endpoint.find(':');
        if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == endpoint.length() - 1) {
            continue;  // Invalid endpoint format
        }
        
        const std::string ip_str = endpoint.substr(0, colon_pos);
        const std::string& ip = (ip_str == "0.0.0.0") ? "127.0.0.1" : ip_str;
        
        try {
            const uint16_t port = static_cast<uint16_t>(std::stoi(endpoint.substr(colon_pos + 1)));
            udp_transport_->send(packet.data(), packet.size(), ip, port);
        } catch (const std::exception&) {
            // Skip invalid port number
            continue;
        }
    }
}

void NodeImpl::handleQuerySubscriptions(const std::string& from_node,
                                       uint16_t from_port,
                                       const std::string& from_addr) {
    // Node is querying our services - reply with all our registered services
    // Send point-to-point to the querying node (not broadcast)
    
    if (!use_udp_ || !udp_transport_ || !udp_transport_->isInitialized()) {
        return;
    }
    
    // Validate from_port
    if (from_port == 0) {
        return;  // Invalid port
    }
    
    // Parse the querying node's address
    const std::string target_ip = (from_addr.empty() || from_addr == "0.0.0.0") ? 
                                  "127.0.0.1" : from_addr;
    
    // Get all services registered by this node from GlobalRegistry
    auto& registry = Nexus::rpc::GlobalRegistry::instance();
    auto all_services = registry.findServices();
    
    // Reply with each of our services via SERVICE_REGISTER (point-to-point send)
    for (const auto& svc : all_services) {
        if (svc.node_id == node_id_ && svc.transport == TransportType::UDP) {
            // Serialize service descriptor to payload
            std::vector<uint8_t> payload;
            payload.push_back(static_cast<uint8_t>(svc.type));
            payload.push_back(static_cast<uint8_t>(svc.transport));
            
            uint8_t channel_len = static_cast<uint8_t>(svc.channel_name.size());
            payload.push_back(channel_len);
            
            uint16_t udp_addr_len = static_cast<uint16_t>(svc.udp_address.size());
            payload.push_back(static_cast<uint8_t>(udp_addr_len & 0xFF));
            payload.push_back(static_cast<uint8_t>((udp_addr_len >> 8) & 0xFF));
            
            if (channel_len > 0) {
                payload.insert(payload.end(), svc.channel_name.begin(), svc.channel_name.end());
            }
            if (udp_addr_len > 0) {
                payload.insert(payload.end(), svc.udp_address.begin(), svc.udp_address.end());
            }
            
            // Build SERVICE_REGISTER packet
            auto packet = MessageBuilder::build(node_id_, svc.group, svc.topic,
                                               payload.data(), payload.size(),
                                               getUdpPort(), MessageType::SERVICE_REGISTER);
            
            // Send directly to the querying node (point-to-point)
            udp_transport_->send(packet.data(), packet.size(), target_ip, from_port);
        }
    }
}

void NodeImpl::handleUdpHeartbeat(const std::string& from_node, 
                                  const std::string& from_addr, 
                                  uint16_t from_port) {
    if (from_node == node_id_) {
        return;  // Ignore self heartbeat
    }
    
    // Update last heartbeat time in remote_nodes_
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto it = remote_nodes_.find(from_node);
    if (it != remote_nodes_.end()) {
        it->second.last_heartbeat = std::chrono::steady_clock::now();
    } else {
        // New UDP node discovered via heartbeat
        RemoteNodeInfo info;
        info.node_id = from_node;
        info.address = from_addr;
        info.port = from_port;
        info.last_heartbeat = std::chrono::steady_clock::now();
        remote_nodes_[from_node] = info;
        
        NEXUS_INFO("IMPL") << "Discovered UDP node via heartbeat: " 
                            << from_node << " at " << from_addr << ":" << from_port;
    }
}

void NodeImpl::checkUdpTimeouts() {
    std::vector<std::string> timed_out_nodes;
    
    {
        std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
        auto now = std::chrono::steady_clock::now();
        
        for (auto it = remote_nodes_.begin(); it != remote_nodes_.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.last_heartbeat).count();
            
            if (elapsed > UDP_TIMEOUT_MS) {
                // Node timed out
                timed_out_nodes.push_back(it->first);
                NEXUS_WARN("IMPL") << "UDP node timed out: " << it->first 
                                    << " (last seen " << elapsed << "ms ago)";
                it = remote_nodes_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Clean up services from timed-out nodes
    if (!timed_out_nodes.empty()) {
        auto& registry = Nexus::rpc::GlobalRegistry::instance();
        
        for (const auto& node_id : timed_out_nodes) {
            // Remove all services from this node
            auto all_services = registry.findServices();
            for (const auto& svc : all_services) {
                if (svc.node_id == node_id && svc.transport == TransportType::UDP) {
                    registry.unregisterService(svc.group, svc);
                    
                    // Trigger SERVICE_REMOVED callback
                    handleServiceUpdate(node_id, svc, false);
                }
            }
            
            // Trigger NODE_LEFT callback
            notifyNodeEvent(ServiceEvent::NODE_LEFT, node_id);
        }
    }
}

// ==================== End UDP Heartbeat ====================

// Factory functions
std::shared_ptr<Node> createNode(const std::string& node_id, TransportMode mode) {
    // Always enable UDP with auto-selected port (0 = auto-select)
    // The framework will automatically choose between in-process and inter-process communication
    auto node = std::make_shared<NodeImpl>(node_id, true, 0, mode);
    std::static_pointer_cast<NodeImpl>(node)->initialize(0);
    return node;
}

} // namespace rpc
} // namespace Nexus
