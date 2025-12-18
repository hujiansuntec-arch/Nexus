#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/core/Config.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace Nexus::rpc;

TEST(NodeUdpTest, InitWithUdp) {
    // Initialize with UDP enabled
    // Use port 0 to let system choose
    auto node = std::make_shared<NodeImpl>("udp_node_test", true, 0);
    node->initialize(0);

    // Check if UDP transport is initialized (indirectly via behavior or logs)
    
    std::vector<std::string> topics = {"topic1"};
    node->subscribe("group1", topics, [](const std::string&, const std::string&, const uint8_t*, size_t) {
    });

    // Allow some time for threads to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(NodeUdpTest, UdpMessageHandling) {
    // This test attempts to cover the UDP receive callback
    // We create two nodes with UDP enabled
    auto node1 = std::make_shared<NodeImpl>("udp_node_1", true, 0);
    node1->initialize(0);

    auto node2 = std::make_shared<NodeImpl>("udp_node_2", true, 0);
    node2->initialize(0);

    std::atomic<int> received_count{0};
    std::vector<std::string> topics = {"udp_topic"};
    
    node2->subscribe("udp_group", topics, [&](const std::string&, const std::string&, const uint8_t*, size_t) {
        received_count++;
    });

    // Wait for discovery
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Publish message
    std::string payload = "hello udp";
    node1->publish("udp_group", "udp_topic", payload);

    // Wait for delivery
    int retries = 0;
    while (received_count == 0 && retries < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retries++;
    }
}
