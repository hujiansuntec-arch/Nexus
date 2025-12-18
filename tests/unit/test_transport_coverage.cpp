#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/core/Config.h"
#include <thread>
#include <chrono>
#include <vector>
#include "nexus/registry/SharedMemoryRegistry.h"

using namespace Nexus::rpc;

TEST(TransportCoverage, ConnectFailures) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    transport.initialize("connector_node", config);
    
    // Manually register a bad node in the registry
    Nexus::rpc::SharedMemoryRegistry registry;
    registry.initialize();
    registry.registerNode("bad_node", "non_existent_shm_file");
    
    // Now try to send to "bad_node". It should try to connect and fail.
    std::vector<uint8_t> data = {1};
    ASSERT_FALSE(transport.send("bad_node", data.data(), data.size()));
    
    // Clean up
    registry.unregisterNode("bad_node");
}

TEST(TransportCoverage, Warmup) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    transport.initialize("warmup_node", config);
    
    transport.warmupConnections();
    // Should not crash.
}

TEST(TransportCoverage, LocalNodes) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    transport.initialize("local_check_node", config);
    
    ASSERT_TRUE(transport.isLocalNode("local_check_node"));
    ASSERT_FALSE(transport.isLocalNode("non_existent"));
    
    auto nodes = transport.getLocalNodes();
    ASSERT_FALSE(nodes.empty());
}


TEST(TransportCoverage, InitializeFailures) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    
    // Invalid node ID
    ASSERT_FALSE(transport.initialize("", config));
    
    // Invalid config (max_inbound_queues too large)
    config.max_inbound_queues = 1000; // Limit is 64
    ASSERT_FALSE(transport.initialize("valid_node", config));
    
    // Reset config
    config.max_inbound_queues = 10;
    
    // Already initialized
    ASSERT_TRUE(transport.initialize("valid_node_init", config));
    ASSERT_TRUE(transport.initialize("valid_node_init", config)); // Should return true immediately
}

TEST(TransportCoverage, SendFailures) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    
    // Not initialized
    std::vector<uint8_t> data = {1, 2, 3};
    ASSERT_FALSE(transport.send("dest", data.data(), data.size()));
    
    transport.initialize("sender_node_fail", config);
    
    // Send to self
    ASSERT_FALSE(transport.send("sender_node_fail", data.data(), data.size()));
    
    // Send to non-existent (should fail to connect, but might return false or log error)
    // Implementation tries to connect. If registry lookup fails, it returns false.
    ASSERT_FALSE(transport.send("non_existent", data.data(), data.size()));
}

TEST(TransportCoverage, ReceiveLoop) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    transport.initialize("receiver_node_loop", config);
    
    // Start receiving (it starts thread)
    transport.startReceiving();
    
    // Stop receiving
    transport.stopReceiving();
    
    // Start again
    transport.startReceiving();
    transport.stopReceiving();
}

TEST(TransportCoverage, Broadcast) {
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    transport.initialize("broadcast_node", config);
    
    std::vector<uint8_t> data = {1, 2, 3};
    // Broadcast returns number of nodes sent to. Should be 0 as we have no connections.
    ASSERT_EQ(transport.broadcast(data.data(), data.size()), 0);
}
