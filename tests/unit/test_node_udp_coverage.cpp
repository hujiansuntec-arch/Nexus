#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/transport/UdpTransport.h"
#include "nexus/core/Message.h"
#include "nexus/registry/GlobalRegistry.h"
#include <thread>
#include <chrono>
#include <vector>
#include <cstring>
#include <atomic>

using namespace Nexus::rpc;

TEST(NodeUdpCoverage, HandleQuerySubscriptions) {
    // 1. Create a node with UDP enabled and force UDP transport mode (disable SHM)
    // This ensures the service is registered with TransportType::UDP
    auto node = std::make_shared<NodeImpl>("udp_query_target", true, 0, TransportMode::UDP);
    node->initialize(0);
    
    // Register a service so it has something to reply with
    std::vector<std::string> topics = {"my_topic"};
    node->subscribe("my_group", topics, [](const std::string&, const std::string&, const uint8_t*, size_t){});
    
    // Debug registry
    auto services = Nexus::rpc::GlobalRegistry::instance().findServices("my_group");
    std::cout << "DEBUG: Services in registry: " << services.size() << std::endl;
    for(const auto& s : services) {
        std::cout << "DEBUG: Service: " << s.node_id << " " << s.topic << " " << (int)s.transport << std::endl;
    }

    uint16_t target_port = node->getUdpPort();
    ASSERT_TRUE(target_port > 0);

    // 2. Create a helper transport to send the query
    UdpTransport helper;
    ASSERT_TRUE(helper.initialize(0)); // Bind to any port
    
    std::atomic<bool> received_reply{false};
    helper.setReceiveCallback([&](const uint8_t* data, size_t size, const std::string&){
        std::cout << "DEBUG: Helper received " << size << " bytes" << std::endl;
        if (size < sizeof(MessagePacket)) return;
        const MessagePacket* packet = reinterpret_cast<const MessagePacket*>(data);
        std::cout << "DEBUG: Msg type: " << (int)packet->msg_type << std::endl;
        if (packet->msg_type == static_cast<uint8_t>(MessageType::SERVICE_REGISTER)) {
            received_reply = true;
        }
    });

    // 3. Send QUERY_SUBSCRIPTIONS packet
    // Payload is empty for query
    auto packet = MessageBuilder::build("helper_node", "", "", nullptr, 0, helper.getPort(), MessageType::QUERY_SUBSCRIPTIONS);
    
    helper.send(packet.data(), packet.size(), "127.0.0.1", target_port);

    // 4. Wait for reply
    for (int i = 0; i < 20; i++) {
        if (received_reply) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ASSERT_TRUE(received_reply);
}

TEST(NodeUdpCoverage, SendHeartbeat) {
    // 1. Create node
    auto node = std::make_shared<NodeImpl>("udp_sender", true, 0);
    node->initialize(0);

    // 2. Create helper transport
    UdpTransport helper;
    ASSERT_TRUE(helper.initialize(0));
    
    // 3. Register a fake UDP service in GlobalRegistry that points to helper
    ServiceDescriptor svc;
    svc.node_id = "fake_receiver";
    svc.group = "test_hb";
    svc.topic = "topic";
    svc.type = ServiceType::NORMAL_MESSAGE;
    svc.transport = TransportType::UDP;
    svc.udp_address = "127.0.0.1:" + std::to_string(helper.getPort());
    
    Nexus::rpc::GlobalRegistry::instance().registerService("test_hb", svc);
    
    // 4. Wait for heartbeat packet on helper
    std::atomic<bool> received_hb{false};
    helper.setReceiveCallback([&](const uint8_t* data, size_t size, const std::string&){
        if (size < sizeof(MessagePacket)) return;
        const MessagePacket* packet = reinterpret_cast<const MessagePacket*>(data);
        if (packet->msg_type == static_cast<uint8_t>(MessageType::HEARTBEAT)) {
            received_hb = true;
        }
    });
    
    // Wait for heartbeat interval (100ms)
    for (int i = 0; i < 20; i++) {
        if (received_hb) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ASSERT_TRUE(received_hb);
    
    // Cleanup
    Nexus::rpc::GlobalRegistry::instance().unregisterService("test_hb", svc);
}

TEST(NodeUdpCoverage, HandleHeartbeatAndTimeout) {
    // 1. Create a node
    auto node = std::make_shared<NodeImpl>("udp_hb_target", true, 0);
    node->initialize(0);
    
    uint16_t target_port = node->getUdpPort();
    
    // 2. Create helper
    UdpTransport helper;
    ASSERT_TRUE(helper.initialize(0));

    // 3. Send HEARTBEAT
    auto packet = MessageBuilder::build("remote_hb_node", "", "", nullptr, 0, helper.getPort(), MessageType::HEARTBEAT);
    helper.send(packet.data(), packet.size(), "127.0.0.1", target_port);

    // 4. Verify timeout triggers NODE_LEFT
    std::atomic<bool> node_left_triggered{false};
    node->setServiceDiscoveryCallback([&](ServiceEvent event, const ServiceDescriptor& svc) {
        if (event == ServiceEvent::NODE_LEFT && svc.node_id == "remote_hb_node") {
            node_left_triggered = true;
        }
    });

    // Wait for timeout (UDP_TIMEOUT_MS = 5000ms)
    // We wait 7s to ensure the periodic check (every 1s) catches it
    printf("Waiting for UDP timeout (5s)...\n");
    for (int i = 0; i < 70; i++) {
        if (node_left_triggered) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    ASSERT_TRUE(node_left_triggered);
}
