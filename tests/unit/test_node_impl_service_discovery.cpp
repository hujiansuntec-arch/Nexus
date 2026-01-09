#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/core/Message.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/registry/GlobalRegistry.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>

using namespace Nexus::rpc;

TEST(NodeImplServiceDiscoveryTest, ServiceQueryAll) {
    // Setup
    Nexus::rpc::GlobalRegistry::instance().clearServices();

    // 1. Create receiver node
    auto receiver = std::make_shared<NodeImpl>("receiver_node", false, 0, TransportMode::LOCK_FREE_SHM);
    receiver->initialize(0);

    // Register a service on receiver
    ServiceDescriptor svc;
    svc.node_id = "receiver_node";
    svc.group = "test_group";
    svc.topic = "test_topic";
    svc.type = ServiceType::NORMAL_MESSAGE;
    svc.transport = TransportType::SHARED_MEMORY;
    Nexus::rpc::GlobalRegistry::instance().registerService("test_group", svc);

    // 2. Create a standalone transport to act as sender
    SharedMemoryTransportV3 sender_transport;
    ASSERT_TRUE(sender_transport.initialize("proc_sender_node"));
    ASSERT_TRUE(sender_transport.registerNodeToRegistry("sender_node"));

    // Setup callback to capture response
    std::atomic<bool> received_response{false};
    std::string received_group;
    std::string received_topic;

    sender_transport.addReceiveCallback("sender_node", [&](const uint8_t* data, size_t size, const std::string& from) {
        if (size < sizeof(MessagePacket)) return;
        const MessagePacket* packet = reinterpret_cast<const MessagePacket*>(data);
        
        if (packet->msg_type == static_cast<uint8_t>(MessageType::SERVICE_REGISTER)) {
            // Verify it's the service we expect
            std::string group(packet->getGroup(), packet->group_len);
            std::string topic(packet->getTopic(), packet->topic_len);
            
            if (group == "test_group" && topic == "test_topic") {
                received_response = true;
                received_group = group;
                received_topic = topic;
            }
        }
    });
    sender_transport.startReceiving();

    // 3. Send SERVICE_REGISTER with empty payload (Query All)
    // Payload length 0 means query
    auto packet = MessageBuilder::build("sender_node", "", "", nullptr, 0, 0, MessageType::SERVICE_REGISTER);
    
    // Send to receiver
    sender_transport.send("receiver_node", packet.data(), packet.size());

    // 4. Wait for response
    int retries = 0;
    while (!received_response && retries++ < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ASSERT_TRUE(received_response);
    ASSERT_EQ(received_group, "test_group");
    ASSERT_EQ(received_topic, "test_topic");

    // Teardown
    receiver.reset();
    Nexus::rpc::GlobalRegistry::instance().clearServices();
}

TEST(NodeImplServiceDiscoveryTest, NodeLeaveCleanup) {
    // Setup
    Nexus::rpc::GlobalRegistry::instance().clearServices();

    // 1. Create receiver node (observer)
    auto observer = std::make_shared<NodeImpl>("observer_node", false, 0, TransportMode::LOCK_FREE_SHM);
    observer->initialize(0);

    // Setup callback to detect node leave
    std::atomic<bool> node_left{false};
    observer->setServiceDiscoveryCallback([&](ServiceEvent event, const ServiceDescriptor& svc) {
        if (event == ServiceEvent::NODE_LEFT && svc.node_id == "leaver_node") {
            node_left = true;
        }
    });

    // 2. Create leaver node
    auto leaver = std::make_shared<NodeImpl>("leaver_node", false, 0, TransportMode::LOCK_FREE_SHM);
    leaver->initialize(0);

    // Register a service on leaver
    ServiceDescriptor svc;
    svc.node_id = "leaver_node";
    svc.group = "leaver_group";
    svc.topic = "leaver_topic";
    svc.type = ServiceType::NORMAL_MESSAGE;
    svc.transport = TransportType::SHARED_MEMORY;
    Nexus::rpc::GlobalRegistry::instance().registerService("leaver_group", svc);

    // Wait for observer to discover leaver (via proactive connection or registry)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. Simulate leaver node leaving
    // Destroying the node triggers unregisterNode() which broadcasts NODE_LEAVE
    leaver.reset();

    // 4. Wait for observer to detect leave
    int retries = 0;
    while (!node_left && retries++ < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ASSERT_TRUE(node_left);

    // Verify service is removed from registry
    auto services = Nexus::rpc::GlobalRegistry::instance().findServices("leaver_group");
    bool service_found = false;
    for (const auto& s : services) {
        if (s.node_id == "leaver_node") {
            service_found = true;
            break;
        }
    }
    ASSERT_FALSE(service_found);

    // Teardown
    observer.reset();
    Nexus::rpc::GlobalRegistry::instance().clearServices();
}
