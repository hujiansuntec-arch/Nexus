#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/core/Message.h"
#include "nexus/transport/UdpTransport.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <iostream>

using namespace Nexus::rpc;

// Helper macros for missing assertions
#define EXPECT_TRUE(condition) ASSERT_TRUE(condition)
#define EXPECT_EQ(a, b) ASSERT_EQ(a, b)

TEST(NodeUdpExtendedTest, HeartbeatTimeout) {
    // Create a node with UDP enabled
    auto node = std::make_shared<NodeImpl>("udp_timeout_node", true, 0);
    node->initialize(0);
    uint16_t node_port = std::static_pointer_cast<NodeImpl>(node)->getUdpPort();
    ASSERT_GT(node_port, 0);

    // Setup callback to detect NODE_LEFT
    std::atomic<bool> node_left_detected{false};
    std::string left_node_id;
    
    node->setServiceDiscoveryCallback([&](ServiceEvent event, const ServiceDescriptor& svc) {
        if (event == ServiceEvent::NODE_LEFT) {
            node_left_detected = true;
            left_node_id = svc.node_id;
        }
    });

    // Create a standalone transport to simulate a remote node
    UdpTransport remote_transport;
    ASSERT_TRUE(remote_transport.initialize(0));

    // Send a HEARTBEAT message
    std::string remote_node_id = "remote_heartbeat_node";
    auto packet = MessageBuilder::build(remote_node_id, "", "", nullptr, 0, remote_transport.getPort(), MessageType::HEARTBEAT);
    
    remote_transport.send(packet.data(), packet.size(), "127.0.0.1", node_port);

    // Wait a bit for the heartbeat to be processed and node registered
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Now wait for timeout (UDP_TIMEOUT_MS = 5000ms) + buffer
    std::cout << "Waiting for UDP timeout (approx 5s)..." << std::endl;
    for (int i = 0; i < 70; ++i) {
        if (node_left_detected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(node_left_detected);
    EXPECT_EQ(left_node_id, remote_node_id);
}

TEST(NodeUdpExtendedTest, QuerySubscriptions) {
    // Create a node with UDP enabled
    auto node = std::make_shared<NodeImpl>("udp_query_node", true, 0);
    node->initialize(0);
    uint16_t node_port = std::static_pointer_cast<NodeImpl>(node)->getUdpPort();

    // Register a service
    std::vector<std::string> topics = {"query_topic"};
    node->subscribe("query_group", topics, [](const std::string&, const std::string&, const uint8_t*, size_t){});
    
    // Allow time for registration
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create a standalone transport to query
    UdpTransport remote_transport;
    ASSERT_TRUE(remote_transport.initialize(0));
    
    std::atomic<bool> response_received{false};
    remote_transport.setReceiveCallback([&](const uint8_t* data, size_t size, const std::string&) {
        if (size < sizeof(MessagePacket)) return;
        const MessagePacket* packet = reinterpret_cast<const MessagePacket*>(data);
        
        if (packet->msg_type == static_cast<uint8_t>(MessageType::SERVICE_REGISTER)) {
            std::string group(packet->getGroup(), packet->group_len);
            std::string topic(packet->getTopic(), packet->topic_len);
            if (group == "query_group" && topic == "query_topic") {
                response_received = true;
            }
        }
    });

    // Send QUERY_SUBSCRIPTIONS
    std::string remote_node_id = "remote_query_node";
    auto packet = MessageBuilder::build(remote_node_id, "", "", nullptr, 0, remote_transport.getPort(), MessageType::QUERY_SUBSCRIPTIONS);
    
    remote_transport.send(packet.data(), packet.size(), "127.0.0.1", node_port);

    // Wait for response
    for (int i = 0; i < 20; ++i) {
        if (response_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(response_received);
}

TEST(NodeUdpExtendedTest, InvalidPackets) {
    // Create a node with UDP enabled
    auto node = std::make_shared<NodeImpl>("udp_invalid_node", true, 0);
    node->initialize(0);
    uint16_t node_port = std::static_pointer_cast<NodeImpl>(node)->getUdpPort();

    // Create a standalone transport
    UdpTransport remote_transport;
    ASSERT_TRUE(remote_transport.initialize(0));

    // 1. Send too small packet
    std::vector<uint8_t> small_packet(5, 0xFF);
    remote_transport.send(small_packet.data(), small_packet.size(), "127.0.0.1", node_port);

    // 2. Send invalid magic
    auto packet = MessageBuilder::build("remote", "g", "t", nullptr, 0, 0, MessageType::DATA);
    MessagePacket* header = reinterpret_cast<MessagePacket*>(packet.data());
    header->magic = 0xDEADBEEF; // Invalid magic
    remote_transport.send(packet.data(), packet.size(), "127.0.0.1", node_port);

    // 3. Send packet with wrong payload length
    packet = MessageBuilder::build("remote", "g", "t", nullptr, 0, 0, MessageType::DATA);
    header = reinterpret_cast<MessagePacket*>(packet.data());
    header->payload_len = 1000; // Claim 1000 bytes but send fewer
    remote_transport.send(packet.data(), packet.size(), "127.0.0.1", node_port);

    // Ensure node is still alive and responsive
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::atomic<bool> msg_received{false};
    node->subscribe("check_group", {"check_topic"}, [&](const std::string&, const std::string&, const uint8_t*, size_t){
        msg_received = true;
    });
    
    // Send a VALID message from remote transport to verify node is still working
    std::string valid_payload = "alive";
    packet = MessageBuilder::build("remote_valid", "check_group", "check_topic", 
                                   reinterpret_cast<const uint8_t*>(valid_payload.data()), valid_payload.size(), 
                                   remote_transport.getPort(), MessageType::DATA);
    remote_transport.send(packet.data(), packet.size(), "127.0.0.1", node_port);
    
    for (int i = 0; i < 10; ++i) {
        if (msg_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_TRUE(msg_received);
}
