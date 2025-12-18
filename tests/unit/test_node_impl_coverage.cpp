#include "simple_test.h"
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include "nexus/core/NodeImpl.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/core/Message.h"
#include "nexus/registry/GlobalRegistry.h"
#include "nexus/registry/SharedMemoryRegistry.h"
#include "nexus/utils/Logger.h"

using namespace Nexus::rpc;

// Helper for cleanup
struct RegistryCleanup {
    RegistryCleanup() { clear(); }
    ~RegistryCleanup() { clear(); }
    
    void clear() {
        GlobalRegistry::instance().clearServices();
        // Force cleanup of shared memory registry
        shm_unlink("/librpc_registry");
    }
};

TEST(NodeImplCoverage, HandleSystemMessages) {
    Nexus::rpc::Logger::instance().setLevel(Nexus::rpc::Logger::Level::DEBUG);
    RegistryCleanup cleanup;
    
    std::string node_id = "test_node_impl_cov";
    auto node = std::make_shared<NodeImpl>(node_id, false, 0);
    node->initialize(0);

    // Create a "remote" transport to send messages to the node
    std::string remote_node_id = "remote_node_sender";
    SharedMemoryTransportV3 remote_transport;
    ASSERT_TRUE(remote_transport.initialize(remote_node_id));

    // 1. Test SERVICE_REGISTER
    std::string group = "test_group";
    std::string topic = "test_topic";
    
    // Construct valid payload
    std::vector<uint8_t> payload_vec;
    payload_vec.push_back(static_cast<uint8_t>(ServiceType::NORMAL_MESSAGE)); // Type
    payload_vec.push_back(static_cast<uint8_t>(TransportType::SHARED_MEMORY)); // Transport
    
    std::string channel_name = "test_channel";
    payload_vec.push_back(static_cast<uint8_t>(channel_name.length())); // Channel len
    
    uint16_t udp_len = 0;
    payload_vec.push_back(static_cast<uint8_t>(udp_len & 0xFF)); // UDP len low
    payload_vec.push_back(static_cast<uint8_t>((udp_len >> 8) & 0xFF)); // UDP len high
    
    payload_vec.insert(payload_vec.end(), channel_name.begin(), channel_name.end());
    
    size_t packet_size = MessagePacket::packetSize(group.length(), topic.length(), payload_vec.size());
    std::vector<uint8_t> buffer(packet_size);
    MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());

    packet->magic = MessagePacket::MAGIC;
    packet->version = MessagePacket::VERSION;
    packet->msg_type = static_cast<uint8_t>(MessageType::SERVICE_REGISTER);
    packet->group_len = group.length();
    packet->topic_len = topic.length();
    packet->payload_len = payload_vec.size();
    packet->udp_port = 0;
    std::strncpy(packet->node_id, remote_node_id.c_str(), sizeof(packet->node_id) - 1);
    
    std::memcpy(packet->getGroup(), group.c_str(), group.length());
    std::memcpy(packet->getTopic(), topic.c_str(), topic.length());
    std::memcpy(packet->getPayload(), payload_vec.data(), payload_vec.size());
    
    packet->checksum = packet->calculateChecksum();

    // Send to node
    ASSERT_TRUE(remote_transport.send(node_id, buffer.data(), buffer.size()));

    // Wait for processing (async system thread)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify service is registered
    auto services = GlobalRegistry::instance().findServices(group);
    bool found = false;
    for (const auto& svc : services) {
        if (svc.topic == topic && svc.node_id == remote_node_id) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    // 2. Test SERVICE_UNREGISTER
    packet->msg_type = static_cast<uint8_t>(MessageType::SERVICE_UNREGISTER);
    packet->checksum = packet->calculateChecksum(); // Recalculate checksum

    remote_transport.send(node_id, buffer.data(), buffer.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    services = GlobalRegistry::instance().findServices(group);
    found = false;
    for (const auto& svc : services) {
        if (svc.topic == topic && svc.node_id == remote_node_id) {
            found = true;
            break;
        }
    }
    ASSERT_FALSE(found);

    // 3. Test NODE_JOIN
    packet->msg_type = static_cast<uint8_t>(MessageType::NODE_JOIN);
    packet->payload_len = 0;
    packet->checksum = packet->calculateChecksum();
    
    size_t join_packet_size = MessagePacket::packetSize(group.length(), topic.length(), 0);
    remote_transport.send(node_id, buffer.data(), join_packet_size);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 4. Test NODE_LEAVE
    packet->msg_type = static_cast<uint8_t>(MessageType::NODE_LEAVE);
    packet->checksum = packet->calculateChecksum();
    remote_transport.send(node_id, buffer.data(), join_packet_size);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(NodeImplCoverage, HandleDataMessage) {
    RegistryCleanup cleanup;

    std::string node_id = "test_node_data_cov";
    auto node = std::make_shared<NodeImpl>(node_id, false, 0);
    node->initialize(0);

    std::string remote_node_id = "remote_node_data";
    SharedMemoryTransportV3 remote_transport;
    ASSERT_TRUE(remote_transport.initialize(remote_node_id));

    std::string group = "data_group";
    std::string topic = "data_topic";
    std::string payload = "hello world";

    bool received = false;
    node->subscribe(group, {topic}, [&](const std::string& g, const std::string& t, const uint8_t* p, size_t s) {
        if (g == group && t == topic && std::string((const char*)p, s) == payload) {
            received = true;
        }
    });

    // Construct DATA packet
    size_t packet_size = MessagePacket::packetSize(group.length(), topic.length(), payload.length());
    std::vector<uint8_t> buffer(packet_size);
    MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());

    packet->magic = MessagePacket::MAGIC;
    packet->version = MessagePacket::VERSION;
    packet->msg_type = static_cast<uint8_t>(MessageType::DATA);
    packet->group_len = group.length();
    packet->topic_len = topic.length();
    packet->payload_len = payload.length();
    packet->udp_port = 0;
    std::strncpy(packet->node_id, remote_node_id.c_str(), sizeof(packet->node_id) - 1);
    
    std::memcpy(packet->getGroup(), group.c_str(), group.length());
    std::memcpy(packet->getTopic(), topic.c_str(), topic.length());
    std::memcpy(packet->getPayload(), payload.c_str(), payload.length());
    
    packet->checksum = packet->calculateChecksum();

    remote_transport.send(node_id, buffer.data(), buffer.size());

    // Wait for processing
    for (int i = 0; i < 10; ++i) {
        if (received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ASSERT_TRUE(received);
}

TEST(NodeImplCoverage, CleanupOrphanedChannels) {
    RegistryCleanup cleanup;
    
    std::string node_id = "cleanup_master_node";
    auto node = std::make_shared<NodeImpl>(node_id, false, 0);
    node->initialize(0);

    // Create a dummy orphaned channel
    {
        LargeDataChannel::Config config;
        config.buffer_size = 1024 * 1024;
        auto channel = LargeDataChannel::create("orphaned_channel", config);
    }
    
    size_t cleaned = node->cleanupOrphanedChannels();
    // Just ensure it runs
    (void)cleaned;
}
