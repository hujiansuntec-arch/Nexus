#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/registry/GlobalRegistry.h"
#include "nexus/core/NodeImpl.h"
#include "nexus/registry/SharedMemoryRegistry.h"
#include <sys/mman.h>
#include <fcntl.h>

using namespace Nexus::rpc;

// Helper for cleanup
struct RegistryCleanup {
    RegistryCleanup() { clear(); }
    ~RegistryCleanup() { clear(); }
    
    void clear() {
        GlobalRegistry::instance().clearServices();
        shm_unlink("/librpc_registry");
    }
};

TEST(TransportV3Coverage, InitializeErrors) {
    RegistryCleanup cleanup;
    SharedMemoryTransportV3 transport;
    
    // 1. Empty node ID
    ASSERT_FALSE(transport.initialize(""));

    // 2. Invalid config
    SharedMemoryTransportV3::Config config;
    config.max_inbound_queues = 100000; // Exceeds MAX_INBOUND_QUEUES (128)
    ASSERT_FALSE(transport.initialize("test_node", config));
}

TEST(TransportV3Coverage, SendErrors) {
    RegistryCleanup cleanup;
    SharedMemoryTransportV3 transport;
    std::string node_id = "sender_node";
    ASSERT_TRUE(transport.initialize(node_id));

    std::vector<uint8_t> data(10, 0);

    // 1. Send to self
    ASSERT_FALSE(transport.send(node_id, data.data(), data.size()));

    // 2. Send to non-existent node
    ASSERT_FALSE(transport.send("non_existent_node", data.data(), data.size()));
}

TEST(TransportV3Coverage, UninitializedUsage) {
    RegistryCleanup cleanup;
    SharedMemoryTransportV3 transport;
    std::vector<uint8_t> data(10, 0);

    ASSERT_FALSE(transport.send("dest", data.data(), data.size()));
    
    ASSERT_TRUE(transport.getLocalNodes().empty());
}

TEST(TransportV3Coverage, DoubleInitialize) {
    RegistryCleanup cleanup;
    SharedMemoryTransportV3 transport;
    ASSERT_TRUE(transport.initialize("node1"));
    ASSERT_TRUE(transport.initialize("node1")); // Should return true (idempotent)
}
