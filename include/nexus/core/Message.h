#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <cstring>

namespace Nexus {
namespace rpc {

/**
 * @brief Message types
 */
enum class MessageType : uint8_t {
    DATA                = 0,  // Normal data broadcast
    SUBSCRIBE           = 1,  // Subscribe registration
    UNSUBSCRIBE         = 2,  // Unsubscribe notification
    QUERY_SUBSCRIPTIONS = 3,  // Query all subscriptions from existing nodes
    SUBSCRIPTION_REPLY  = 4,  // Reply with subscription information
    SERVICE_REGISTER    = 5,  // Service registration (for service discovery)
    SERVICE_UNREGISTER  = 6,  // Service unregistration (for service discovery)
    NODE_JOIN           = 7,  // Node joined notification
    NODE_LEAVE          = 8,  // Node leaving notification
    HEARTBEAT           = 9,  // Heartbeat (for UDP node liveness detection)
};

/**
 * @brief Message packet structure for network transmission
 */
struct MessagePacket {
    static constexpr uint32_t MAGIC = 0x4C525043; // "LRPC"
    static constexpr uint16_t VERSION = 1;
    static constexpr uint16_t MAX_GROUP_LEN = 128;
    static constexpr uint16_t MAX_TOPIC_LEN = 128;
    static constexpr uint32_t MAX_PAYLOAD_LEN = 65000; // ~64KB
    
    uint32_t magic;          // Magic number for validation
    uint16_t version;        // Protocol version
    uint8_t msg_type;        // Message type (DATA, SUBSCRIBE, UNSUBSCRIBE)
    uint8_t reserved;        // Reserved for future use
    uint16_t group_len;      // Length of group name
    uint16_t topic_len;      // Length of topic name
    uint32_t payload_len;    // Length of payload
    uint32_t checksum;       // Simple checksum
    char node_id[64];        // Source node ID
    uint16_t udp_port;       // Sender's UDP port
    char data[];             // Variable length data: [group][topic][payload]
    
    // Calculate total packet size
    static size_t packetSize(size_t group_len, size_t topic_len, size_t payload_len) {
        return sizeof(MessagePacket) + group_len + topic_len + payload_len;
    }
    
    // Get group pointer
    const char* getGroup() const { return data; }
    char* getGroup() { return data; }
    
    // Get topic pointer
    const char* getTopic() const { return data + group_len; }
    char* getTopic() { return data + group_len; }
    
    // Get payload pointer
    const uint8_t* getPayload() const { 
        return reinterpret_cast<const uint8_t*>(data + group_len + topic_len); 
    }
    uint8_t* getPayload() { 
        return reinterpret_cast<uint8_t*>(data + group_len + topic_len); 
    }
    
    // Calculate checksum
    uint32_t calculateChecksum() const {
        uint32_t sum = 0;
        sum += magic;
        sum += version;
        sum += msg_type;
        sum += group_len;
        sum += topic_len;
        sum += payload_len;
        sum += udp_port;
        
        // Simple hash of node_id
        for (size_t i = 0; i < sizeof(node_id) && node_id[i]; ++i) {
            sum += static_cast<uint8_t>(node_id[i]);
        }
        
        // Hash of data
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
        size_t total_len = group_len + topic_len + payload_len;
        for (size_t i = 0; i < total_len; ++i) {
            sum += ptr[i];
        }
        
        return sum;
    }
    
    // Validate packet
    bool isValid() const {
        if (magic != MAGIC) return false;
        if (version != VERSION) return false;
        if (group_len > MAX_GROUP_LEN) return false;
        if (topic_len > MAX_TOPIC_LEN) return false;
        if (payload_len > MAX_PAYLOAD_LEN) return false;
        return checksum == calculateChecksum();
    }
};

/**
 * @brief Subscription key for routing
 */
struct SubscriptionKey {
    std::string group;
    std::string topic;
    
    SubscriptionKey(const std::string& g, const std::string& t)
        : group(g), topic(t) {}
    
    bool operator<(const SubscriptionKey& other) const {
        if (group != other.group) return group < other.group;
        return topic < other.topic;
    }
    
    bool operator==(const SubscriptionKey& other) const {
        return group == other.group && topic == other.topic;
    }
    
    std::string toString() const {
        return group + "/" + topic;
    }
};

/**
 * @brief Message builder for creating packets
 */
class MessageBuilder {
public:
    static std::vector<uint8_t> build(const std::string& node_id,
                                      const std::string& group,
                                      const std::string& topic,
                                      const std::string& payload,
                                      uint16_t udp_port = 0,
                                      MessageType msg_type = MessageType::DATA) {
        return build(node_id, group, topic, 
                    reinterpret_cast<const uint8_t*>(payload.data()), 
                    payload.size(), udp_port, msg_type);
    }
    
    static std::vector<uint8_t> build(const std::string& node_id,
                                      const std::string& group,
                                      const std::string& topic,
                                      const uint8_t* payload,
                                      size_t payload_len,
                                      uint16_t udp_port = 0,
                                      MessageType msg_type = MessageType::DATA) {
        size_t total_size = MessagePacket::packetSize(group.size(), topic.size(), payload_len);
        std::vector<uint8_t> buffer(total_size);
        
        MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());
        packet->magic = MessagePacket::MAGIC;
        packet->version = MessagePacket::VERSION;
        packet->msg_type = static_cast<uint8_t>(msg_type);
        packet->reserved = 0;
        packet->group_len = static_cast<uint16_t>(group.size());
        packet->topic_len = static_cast<uint16_t>(topic.size());
        packet->payload_len = static_cast<uint32_t>(payload_len);
        packet->udp_port = udp_port;
        
        // Copy node_id (truncate if too long)
        std::strncpy(packet->node_id, node_id.c_str(), sizeof(packet->node_id) - 1);
        packet->node_id[sizeof(packet->node_id) - 1] = '\0';
        
        // Copy data
        std::memcpy(packet->getGroup(), group.data(), group.size());
        std::memcpy(packet->getTopic(), topic.data(), topic.size());
        std::memcpy(packet->getPayload(), payload, payload_len);
        
        // Calculate checksum
        packet->checksum = packet->calculateChecksum();
        
        return buffer;
    }
};

} // namespace rpc
} // namespace Nexus
