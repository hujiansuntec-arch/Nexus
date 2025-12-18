#include "simple_test.h"
#include "nexus/registry/GlobalRegistry.h"
#include "nexus/registry/SharedMemoryRegistry.h"
#include "nexus/core/NodeImpl.h"
#include <memory>
#include <unistd.h> // For getpid()

using namespace Nexus;
using namespace Nexus::rpc;

// Mock Node for Registry testing
class MockNode : public NodeImpl {
public:
    MockNode(const std::string& id) : NodeImpl(id, false, 0, TransportMode::AUTO) {}
    
    // NodeImpl implements all virtuals from Node, so we don't need to override anything
    // unless we want to change behavior. For registry testing, default behavior is fine.
};

TEST(GlobalRegistryTest, RegisterNode) {
    auto& registry = GlobalRegistry::instance();
    auto node = std::make_shared<MockNode>("node1");
    
    // Pass weak_ptr as required by registerNode
    registry.registerNode("node1", std::weak_ptr<NodeImpl>(node));
    
    auto found = registry.findNode("node1");
    ASSERT_TRUE(found != nullptr);
    ASSERT_EQ("node1", found->getNodeId());
    
    registry.unregisterNode("node1");
    found = registry.findNode("node1");
    ASSERT_TRUE(found == nullptr);
}

TEST(SharedMemoryRegistryTest, BasicOperations) {
    SharedMemoryRegistry::cleanupOrphanedRegistry();
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    std::string node_id = "test_node_shm_reg";
    // registerNode takes (node_id, shm_name)
    registry.registerNode(node_id, "/test_shm");
    
    ASSERT_TRUE(registry.nodeExists(node_id));
    
    NodeInfo info;
    ASSERT_TRUE(registry.findNode(node_id, info));
    ASSERT_EQ(node_id, info.node_id);
    
    registry.unregisterNode(node_id);
    ASSERT_FALSE(registry.nodeExists(node_id));
}

TEST(SharedMemoryRegistryTest, HeartbeatAndCleanup) {
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    std::string node_id = "hb_node";
    registry.registerNode(node_id, "/hb_shm");
    
    // Update heartbeat
    ASSERT_TRUE(registry.updateHeartbeat(node_id));
    
    // Verify node info has updated timestamp (hard to verify exact time, but call should succeed)
    NodeInfo info;
    ASSERT_TRUE(registry.findNode(node_id, info));
    
    // List nodes
    auto nodes = registry.getAllNodes();
    bool found = false;
    for (const auto& n : nodes) {
        if (n.node_id == node_id) found = true;
    }
    ASSERT_TRUE(found);
    
    // Unregister
    registry.unregisterNode(node_id);
    ASSERT_FALSE(registry.nodeExists(node_id));
}

TEST(SharedMemoryRegistryTest, MaxNodes) {
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    // Try to register many nodes
    // Assuming MAX_NODES is reasonable (e.g. 64 or 128)
    // We won't fill it all, but we can register a few more.
    
    for (int i = 0; i < 10; ++i) {
        std::string id = "node_" + std::to_string(i);
        registry.registerNode(id, "/shm_" + std::to_string(i));
    }
    
    auto nodes = registry.getAllNodes();
    ASSERT_TRUE(nodes.size() >= 10);
    
    // Cleanup
    for (int i = 0; i < 10; ++i) {
        std::string id = "node_" + std::to_string(i);
        registry.unregisterNode(id);
    }
}
