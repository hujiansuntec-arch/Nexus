#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/core/Message.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>

using namespace Nexus::rpc;

// Helper to allocate packet with payload
std::vector<uint8_t> createPacket(MessageType type, const std::string& node_id, const std::string& payload = "") {
    size_t size = MessagePacket::packetSize(0, 0, payload.length());
    std::vector<uint8_t> buffer(size);
    MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());
    
    std::memset(packet, 0, sizeof(MessagePacket));
    packet->magic = MessagePacket::MAGIC;
    packet->version = MessagePacket::VERSION;
    packet->msg_type = static_cast<uint8_t>(type);
    std::strncpy(packet->node_id, node_id.c_str(), sizeof(packet->node_id) - 1);
    packet->payload_len = payload.length();
    
    if (!payload.empty()) {
        std::memcpy(packet->getPayload(), payload.c_str(), payload.length());
    }
    
    packet->checksum = packet->calculateChecksum();
    return buffer;
}

TEST(NodeImplCoverage, InitializeWithUdp) {
    // Test initialization with UDP enabled
    auto node = std::make_shared<NodeImpl>("test_udp_node", true, 48000, TransportMode::AUTO);
    node->initialize(48000);
    
    // Allow some time for threads to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(NodeImplCoverage, HandleAllMessageTypes) {
    // Create a node
    std::string node_id = "test_msg_node";
    auto node = std::make_shared<NodeImpl>(node_id, false, 0, TransportMode::LOCK_FREE_SHM);
    node->initialize(0);

    SharedMemoryTransportV3 sender;
    ASSERT_TRUE(sender.initialize("sender_node"));
    
    // Connect to the target node
    std::vector<uint8_t> dummy(1, 0);
    sender.send(node_id, dummy.data(), 0); // Trigger connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send various message types
    auto send = [&](MessageType type, const std::string& payload = "") {
        auto buffer = createPacket(type, "sender_node", payload);
        sender.send(node_id, buffer.data(), buffer.size());
    };

    send(MessageType::NODE_JOIN);
    send(MessageType::NODE_LEAVE);
    send(MessageType::SERVICE_REGISTER, "test/service");
    send(MessageType::SERVICE_UNREGISTER, "test/service");
    send(MessageType::HEARTBEAT); // Should be ignored by SHM
    
    // Send invalid message (wrong magic)
    {
        auto buffer = createPacket(MessageType::DATA, "sender_node");
        MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());
        packet->magic = 0xDEADBEEF; // Wrong magic
        sender.send(node_id, buffer.data(), buffer.size());
    }

    // Send message from self (should be ignored)
    {
        auto buffer = createPacket(MessageType::DATA, node_id); // Self ID
        sender.send(node_id, buffer.data(), buffer.size());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST(NodeImplCoverage, SystemMessageQueue) {
    // Test the system message processing loop
    auto node = std::make_shared<NodeImpl>("test_sys_node", false, 0, TransportMode::LOCK_FREE_SHM);
    node->initialize(0);
    
    SharedMemoryTransportV3 sender;
    ASSERT_TRUE(sender.initialize("sys_sender"));
    
    // Send a burst of SERVICE_REGISTER messages
    for (int i = 0; i < 50; ++i) {
        auto buffer = createPacket(MessageType::SERVICE_REGISTER, "sys_sender", "service/" + std::to_string(i));
        sender.send("test_sys_node", buffer.data(), buffer.size());
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}
