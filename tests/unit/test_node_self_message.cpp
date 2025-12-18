#include "simple_test.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include <thread>
#include <chrono>

using namespace Nexus::rpc;

TEST(NodeSelfMessageTest, IgnoreSelfMessage) {
    std::string node_id = "self_msg_node";
    auto node = std::make_shared<NodeImpl>(node_id, false, 0);
    node->initialize(0);

    // Create a separate transport to inject a message
    std::string injector_id = "injector_node";
    SharedMemoryTransportV3 injector;
    if (injector.initialize(injector_id)) {
        // ...
    }
}

TEST(NodeSelfMessageTest, TryInitWithSameId) {
    std::string node_id = "double_init_node";
    auto node = std::make_shared<NodeImpl>(node_id, false, 0);
    node->initialize(0);
    
    SharedMemoryTransportV3 injector;
    if (injector.initialize(node_id)) {
        std::vector<uint8_t> data = {0xDE, 0xAD};
        // Send to ourselves
        injector.send(node_id, data.data(), data.size());
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
