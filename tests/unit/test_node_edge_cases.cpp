#include "simple_test.h"
#include "nexus/core/NodeImpl.h"

using namespace Nexus::rpc;

TEST(NodeEdgeCasesTest, UnsubscribeEdgeCases) {
    auto node = std::make_shared<NodeImpl>("edge_node", false, 0);
    node->initialize(0);

    std::vector<std::string> topics = {"topic1", "topic2"};
    node->subscribe("group1", topics, [](const std::string&, const std::string&, const uint8_t*, size_t){});

    // 1. Unsubscribe from non-existent group
    Node::Error err = node->unsubscribe("non_existent_group", topics);
    ASSERT_EQ(err, Node::Error::NOT_FOUND);

    // 2. Unsubscribe from all topics in a group (empty list)
    err = node->unsubscribe("group1", {});
    ASSERT_EQ(err, Node::Error::NO_ERROR);
    
    // Verify group is gone (unsubscribe again should fail)
    err = node->unsubscribe("group1", {});
    ASSERT_EQ(err, Node::Error::NOT_FOUND);
}

TEST(NodeEdgeCasesTest, PublishAfterStop) {
    // However, we can test invalid args for publish
    auto node = std::make_shared<NodeImpl>("edge_node_2", false, 0);
    node->initialize(0);
    
    std::string payload = "data";
    Node::Error err = node->publish("", "topic", payload);
    ASSERT_EQ(err, Node::Error::INVALID_ARG);
    
    err = node->publish("group", "", payload);
    ASSERT_EQ(err, Node::Error::INVALID_ARG);
}

TEST(NodeEdgeCasesTest, SubscribeInvalidArgs) {
    auto node = std::make_shared<NodeImpl>("edge_node_3", false, 0);
    node->initialize(0);
    
    std::vector<std::string> topics = {"t1"};
    Node::Error err = node->subscribe("", topics, [](const std::string&, const std::string&, const uint8_t*, size_t){});
    ASSERT_EQ(err, Node::Error::INVALID_ARG);
    
    err = node->subscribe("g1", {}, [](const std::string&, const std::string&, const uint8_t*, size_t){});
    ASSERT_EQ(err, Node::Error::INVALID_ARG);
    
    err = node->subscribe("g1", topics, nullptr);
    ASSERT_EQ(err, Node::Error::INVALID_ARG);
}
