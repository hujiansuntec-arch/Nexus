#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/core/Message.h"
#include <thread>
#include <vector>

using namespace Nexus;
using namespace Nexus::rpc;

TEST(NodeImplExtendedTest, ReceiveDifferentMessageTypes) {
    auto node = std::make_shared<NodeImpl>("node_recv_types", false, 0, TransportMode::AUTO);
    node->initialize(0);
    
    SharedMemoryTransportV3 sender;
    ASSERT_TRUE(sender.initialize("sender_node"));
    
    // Wait for discovery
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send Heartbeat
    {
        std::vector<uint8_t> buffer(sizeof(MessagePacket));
        MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());
        packet->magic = MessagePacket::MAGIC;
        packet->version = 1;
        packet->msg_type = static_cast<uint8_t>(MessageType::HEARTBEAT);
        packet->payload_len = 0;
        
        sender.send("node_recv_types", buffer.data(), buffer.size());
    }
    
    // Send Subscribe (should be ignored but cover the case)
    {
        std::vector<uint8_t> buffer(sizeof(MessagePacket));
        MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());
        packet->magic = MessagePacket::MAGIC;
        packet->version = 1;
        packet->msg_type = static_cast<uint8_t>(MessageType::SUBSCRIBE);
        packet->payload_len = 0;
        
        sender.send("node_recv_types", buffer.data(), buffer.size());
    }
    
    // Send Invalid Magic
    {
        std::vector<uint8_t> buffer(sizeof(MessagePacket));
        MessagePacket* packet = reinterpret_cast<MessagePacket*>(buffer.data());
        packet->magic = 0xDEADBEEF; // Invalid
        packet->version = 1;
        packet->msg_type = static_cast<uint8_t>(MessageType::DATA);
        packet->payload_len = 0;
        
        sender.send("node_recv_types", buffer.data(), buffer.size());
    }
    
    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
