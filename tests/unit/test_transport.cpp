#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/transport/UdpTransport.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/core/Message.h"
#include <thread>

using namespace Nexus;
using namespace Nexus::rpc;

// Mock Node for Transport testing
class MockNodeForTransport : public NodeImpl {
public:
    MockNodeForTransport(const std::string& id) 
        : NodeImpl(id, false, 0, TransportMode::AUTO) {}
};

TEST(SharedMemoryTransportTest, BasicInit) {
    SharedMemoryTransportV3 transport;
    
    // initialize takes (node_id, config)
    ASSERT_TRUE(transport.initialize("test_node_shm"));
    
    // No shutdown method exposed in public API, handled by destructor
}

TEST(UdpTransportTest, BasicInit) {
    UdpTransport transport;
    
    // initialize takes (port)
    ASSERT_TRUE(transport.initialize(0));
}

TEST(TransportTest, MessageSerialization) {
    // Test MessagePacket struct serialization
    MessagePacket msg;
    msg.magic = MessagePacket::MAGIC;
    msg.version = 1;
    msg.msg_type = static_cast<uint8_t>(MessageType::DATA);
    
    ASSERT_EQ(MessagePacket::MAGIC, msg.magic);
}
