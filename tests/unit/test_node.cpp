#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include <atomic>
#include <thread>

using namespace Nexus;
using namespace Nexus::rpc;

TEST(NodeImplTest, Lifecycle) {
    auto node = std::make_shared<NodeImpl>("test_node_1", false, 0, TransportMode::AUTO);
    node->initialize(0);
    
    ASSERT_EQ("test_node_1", node->getNodeId());
    
    // Destructor will be called when node goes out of scope
}

TEST(NodeImplTest, PubSubInProcess) {
    // Use AUTO mode, but since we are in same process, it should use in-process delivery
    auto node1 = std::make_shared<NodeImpl>("node1", false, 0, TransportMode::AUTO);
    node1->initialize(0);
    
    auto node2 = std::make_shared<NodeImpl>("node2", false, 0, TransportMode::AUTO);
    node2->initialize(0);
    
    std::atomic<int> received_count{0};
    std::string last_msg;
    
    node2->subscribe("group1", {"topic1"}, [&](const std::string& group, const std::string& topic, const uint8_t* data, size_t size) {
        ASSERT_EQ("group1", group);
        ASSERT_EQ("topic1", topic);
        last_msg.assign(reinterpret_cast<const char*>(data), size);
        received_count++;
    });
    
    // Wait for subscription to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::string msg = "Hello World";
    node1->publish("group1", "topic1", msg);
    
    // Wait for delivery
    for (int i = 0; i < 10; ++i) {
        if (received_count > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    ASSERT_EQ(1, received_count);
    ASSERT_EQ(msg, last_msg);
}

TEST(NodeImplTest, Unsubscribe) {
    auto node = std::make_shared<NodeImpl>("node1", false, 0, TransportMode::AUTO);
    node->initialize(0);
    
    std::atomic<int> count{0};
    auto cb = [&](const std::string&, const std::string&, const uint8_t*, size_t) { count++; };
    
    node->subscribe("g1", {"t1"}, cb);
    ASSERT_TRUE(node->isSubscribed("g1", "t1"));
    
    node->unsubscribe("g1", {"t1"});
    ASSERT_FALSE(node->isSubscribed("g1", "t1"));
    
    // Publish to self (should not receive if unsubscribed)
    node->publish("g1", "t1", "data");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(0, count);
}

TEST(NodeImplTest, AutoGenerateId) {
    auto node = std::make_shared<NodeImpl>("", false, 0, TransportMode::AUTO);
    node->initialize(0);
    
    std::string id = node->getNodeId();
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(id.find("node_") == 0);
}

TEST(NodeImplTest, UdpTransport) {
    // Use a random port to avoid conflicts
    auto node = std::make_shared<NodeImpl>("udp_node", true, 0, TransportMode::AUTO);
    node->initialize(0);
    
    // Just verify it initializes without crash
    ASSERT_EQ("udp_node", node->getNodeId());
}

TEST(NodeImplTest, SelfMessage) {
    auto node = std::make_shared<NodeImpl>("self_node", false, 0, TransportMode::AUTO);
    node->initialize(0);
    
    std::atomic<int> count{0};
    node->subscribe("g1", {"t1"}, [&](const std::string&, const std::string&, const uint8_t*, size_t) {
        count++;
    });
    
    // Wait for subscription
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    node->publish("g1", "t1", "hello");
    
    // Wait for delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // NodeImpl filters self messages
    ASSERT_EQ(0, count);
}
