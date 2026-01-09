#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/core/Message.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/transport/LargeDataChannel.h"
#include "nexus/core/Config.h"
#include <thread>
#include <chrono>
#include <memory>
#include <string>

using namespace Nexus::rpc;

// Helper class to manage node lifecycle
class NodeGapTestHelper {
public:
    NodeGapTestHelper() {
        node_id_ = "test_gap_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        // Fix constructor arguments: node_id, use_udp, udp_port, transport_mode
        node_ = std::make_shared<NodeImpl>(node_id_, false, 0, TransportMode::AUTO);
        node_->initialize(0);
    }

    ~NodeGapTestHelper() {
        node_.reset();
    }

    std::string node_id_;
    std::shared_ptr<NodeImpl> node_;
};

TEST(NodeImplCoverageGapTest, PublishInvalidArgs) {
    NodeGapTestHelper helper;
    std::string data = "123";
    
    ASSERT_EQ(helper.node_->publish("", "topic", data), Node::Error::INVALID_ARG);
    ASSERT_EQ(helper.node_->publish("group", "", data), Node::Error::INVALID_ARG);
    
    std::string empty_data;
    ASSERT_EQ(helper.node_->publish("group", "topic", empty_data), Node::Error::NO_ERROR);
}

TEST(NodeImplCoverageGapTest, SendLargeDataInvalidArgs) {
    NodeGapTestHelper helper;
    std::vector<uint8_t> data = {1, 2, 3};
    
    ASSERT_EQ(helper.node_->sendLargeData("", "channel", "topic", data.data(), data.size()), Node::Error::INVALID_ARG);
    ASSERT_EQ(helper.node_->sendLargeData("group", "", "topic", data.data(), data.size()), Node::Error::INVALID_ARG);
    ASSERT_EQ(helper.node_->sendLargeData("group", "channel", "", data.data(), data.size()), Node::Error::INVALID_ARG);
    ASSERT_EQ(helper.node_->sendLargeData("group", "channel", "topic", nullptr, data.size()), Node::Error::INVALID_ARG);
    ASSERT_EQ(helper.node_->sendLargeData("group", "channel", "topic", data.data(), 0), Node::Error::INVALID_ARG);
}

TEST(NodeImplCoverageGapTest, UnusedMessageTypes) {
    NodeGapTestHelper helper;
    
    // Create a separate transport to send messages to the node
    SharedMemoryTransportV3 sender;
    std::string sender_id = "sender_" + helper.node_id_;
    ASSERT_TRUE(sender.initialize(sender_id));

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Helper to send a message of a specific type
    auto sendMsg = [&](MessageType type) {
        auto packet = MessageBuilder::build(sender_id, "group", "topic", nullptr, 0, 0, type);
        sender.send(helper.node_id_, packet.data(), packet.size());
    };

    sendMsg(MessageType::SUBSCRIBE);
    sendMsg(MessageType::UNSUBSCRIBE);
    sendMsg(MessageType::QUERY_SUBSCRIPTIONS);
    sendMsg(MessageType::SUBSCRIPTION_REPLY);
    sendMsg(MessageType::HEARTBEAT); // Should be ignored

    // Give some time for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

TEST(NodeImplCoverageGapTest, InvalidPacket) {
    NodeGapTestHelper helper;
    
    SharedMemoryTransportV3 sender;
    std::string sender_id = "sender_inv_" + helper.node_id_;
    ASSERT_TRUE(sender.initialize(sender_id));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send too small packet
    std::vector<uint8_t> small_data = {0, 1, 2};
    sender.send(helper.node_id_, small_data.data(), small_data.size());

    // Send invalid magic
    auto packet = MessageBuilder::build(sender_id, "group", "topic", nullptr, 0, 0, MessageType::DATA);
    // Corrupt magic (first 4 bytes)
    MessagePacket* pkt = reinterpret_cast<MessagePacket*>(packet.data());
    pkt->magic = 0xDEADBEEF; 
    
    sender.send(helper.node_id_, packet.data(), packet.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(NodeImplCoverageGapTest, SelfMessage) {
    NodeGapTestHelper helper;
    
    SharedMemoryTransportV3 sender;
    std::string sender_id = "sender_self_" + helper.node_id_;
    ASSERT_TRUE(sender.initialize(sender_id));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Build packet with node_id_ as source
    auto packet = MessageBuilder::build(helper.node_id_, "group", "topic", nullptr, 0, 0, MessageType::DATA);
    sender.send(helper.node_id_, packet.data(), packet.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(NodeImplCoverageGapTest, UnsubscribeNotFound) {
    NodeGapTestHelper helper;
    std::vector<std::string> topics = {"topic1"};
    ASSERT_EQ(helper.node_->unsubscribe("non_existent_group", topics), Node::Error::NOT_FOUND);
}

TEST(NodeImplCoverageGapTest, SubscribeEmptyTopics) {
    NodeGapTestHelper helper;
    std::vector<std::string> topics = {"", "valid_topic", ""};
    
    // Should succeed but ignore empty topics
    ASSERT_EQ(helper.node_->subscribe("group", topics, [](const std::string&, const std::string&, const uint8_t*, size_t){}), Node::Error::NO_ERROR);
    
    // Verify only valid_topic is subscribed
    ASSERT_TRUE(helper.node_->isSubscribed("group", "valid_topic"));
    ASSERT_FALSE(helper.node_->isSubscribed("group", ""));
}

TEST(NodeImplCoverageGapTest, SendLargeDataFullBuffer) {
    NodeGapTestHelper helper;
    std::string channel_name = "full_channel_" + helper.node_id_;
    std::string topic = "topic";
    std::string group = "group";
    
    // Create a receiver node to act as a reader
    NodeGapTestHelper receiver;
    // Ensure receiver connects to the same channel
    auto recv_channel = receiver.node_->getLargeDataChannel(channel_name);
    ASSERT_NE(recv_channel, nullptr);
    
    // Force registration of the reader by attempting a read
    // This is necessary because getLargeDataChannel only opens the SHM, 
    // but doesn't register as a reader until the first read attempt.
    Nexus::rpc::LargeDataChannel::DataBlock block;
    recv_channel->tryRead(block); 
    
    // Create a large payload (e.g. 8MB)
    size_t payload_size = 8 * 1024 * 1024; 
    std::vector<uint8_t> data(payload_size, 0xFF);
    
    // Send repeatedly until it fills up
    // Default buffer is 64MB, so ~8 writes should fill it
    int success_count = 0;
    Node::Error err = Node::Error::NO_ERROR;
    
    for (int i = 0; i < 20; ++i) {
        err = helper.node_->sendLargeData(group, channel_name, topic, data.data(), data.size());
        if (err != Node::Error::NO_ERROR) {
            break;
        }
        success_count++;
    }
    
    // Should eventually fail with TIMEOUT (buffer full)
    ASSERT_TRUE(err == Node::Error::TIMEOUT || err == Node::Error::UNEXPECTED_ERROR);
    ASSERT_GT(success_count, 0);
}

TEST(NodeImplCoverageGapTest, QueueOverflow) {
    // Save original config
    auto& config = Nexus::rpc::Config::instance();
    size_t original_size = config.node.max_queue_size;
    config.node.max_queue_size = 1;
    
    NodeGapTestHelper receiver;
    
    std::atomic<int> overflow_count{0};
    receiver.node_->setQueueOverflowCallback([&](const std::string&, const std::string&, size_t) {
        overflow_count++;
    });
    
    receiver.node_->setQueueOverflowPolicy(QueueOverflowPolicy::DROP_NEWEST);
    
    receiver.node_->subscribe("group", {"topic"}, [](const std::string&, const std::string&, const uint8_t*, size_t){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    
    NodeGapTestHelper sender;
    // Wait for discovery
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    std::string payload = "data";
    for (int i = 0; i < 20; ++i) {
        sender.node_->publish("group", "topic", payload);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    config.node.max_queue_size = original_size;
    
    ASSERT_GT(overflow_count, 0);
}

TEST(NodeImplCoverageGapTest, QueueOverflowDropOldest) {
    auto& config = Nexus::rpc::Config::instance();
    size_t original_size = config.node.max_queue_size;
    config.node.max_queue_size = 1;
    
    NodeGapTestHelper receiver;
    std::atomic<int> overflow_count{0};
    receiver.node_->setQueueOverflowCallback([&](const std::string&, const std::string&, size_t) {
        overflow_count++;
    });
    
    receiver.node_->setQueueOverflowPolicy(QueueOverflowPolicy::DROP_OLDEST);
    
    receiver.node_->subscribe("group", {"topic"}, [](const std::string&, const std::string&, const uint8_t*, size_t){
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    
    NodeGapTestHelper sender;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    std::string payload = "data";
    for (int i = 0; i < 20; ++i) {
        sender.node_->publish("group", "topic", payload);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    config.node.max_queue_size = original_size;
    
    ASSERT_GT(overflow_count, 0);
}
