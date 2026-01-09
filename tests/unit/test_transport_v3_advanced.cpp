#include "simple_test.h"
#define private public
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/registry/SharedMemoryRegistry.h"
#undef private
#include <thread>
#include <vector>
#include <atomic>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

using namespace Nexus::rpc;

// Helper to access private members
class TransportV3TestHelper : public SharedMemoryTransportV3 {
public:
    using SharedMemoryTransportV3::SharedMemoryTransportV3;

    bool test_initialize(const std::string& id) {
        Config config;
        return initialize(id, config);
    }

    bool test_send(const std::string& dest, const uint8_t* data, size_t size) {
        return send(dest, data, size);
    }

    int test_broadcast(const uint8_t* data, size_t size) {
        return broadcast(data, size);
    }
    
    void test_cleanup() {
        cleanupOrphanedMemory();
    }

    bool test_connectToNode(const std::string& target_node_id) {
        return connectToNode(target_node_id);
    }
};

TEST(TransportV3AdvancedTest, InitializeFailures) {
    TransportV3TestHelper transport;
    
    // Empty ID
    Nexus::rpc::SharedMemoryTransportV3::Config config;
    ASSERT_FALSE(transport.initialize("", config));
    
    // Invalid config
    config.max_inbound_queues = 10000; // Too big
    ASSERT_FALSE(transport.initialize("node1", config));
}

TEST(TransportV3AdvancedTest, SendFailures) {
    TransportV3TestHelper transport;
    Nexus::rpc::SharedMemoryTransportV3::Config config;
    transport.initialize("node_send_test", config);
    
    // Send to self
    std::vector<uint8_t> data(10, 0);
    ASSERT_FALSE(transport.test_send("node_send_test", data.data(), data.size()));
    
    // Send to non-existent
    ASSERT_FALSE(transport.test_send("non_existent_node", data.data(), data.size()));
}

TEST(TransportV3AdvancedTest, BroadcastSkipSelf) {
    TransportV3TestHelper transport;
    Nexus::rpc::SharedMemoryTransportV3::Config config;
    transport.initialize("node_bcast_test", config);
    
    // Broadcast should return 0 if no other nodes
    std::vector<uint8_t> data(10, 0);
    ASSERT_EQ(transport.test_broadcast(data.data(), data.size()), 0);
}

TEST(TransportV3AdvancedTest, CleanupOrphaned) {
    // Create a dummy orphaned file
    std::string name = "/librpc_node_orphan_test";
    std::string path = "/dev/shm" + name;
    
    int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
    ASSERT_GT(fd, 0);
    
    size_t size = sizeof(Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory);
    ftruncate(fd, size);
    
    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(addr, MAP_FAILED);
    
    auto* shm = static_cast<Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory*>(addr);
    shm->header.magic.store(Nexus::rpc::SharedMemoryTransportV3::MAGIC);
    shm->header.owner_pid.store(999999); // Dead PID
    shm->header.num_accessors.store(0);
    
    munmap(addr, size);
    close(fd);
    
    // Run cleanup
    TransportV3TestHelper::cleanupOrphanedMemory();
    
    // Verify file is gone (shm_unlink should have been called)
    // Note: shm_unlink removes the name, but if we don't check immediately it might be tricky.
    // But cleanupOrphanedMemory logs "Unlinked shared memory".
    
    // We can check if shm_open fails
    int fd2 = shm_open(name.c_str(), O_RDONLY, 0666);
    if (fd2 >= 0) {
        close(fd2);
        shm_unlink(name.c_str()); // Cleanup if test failed
        throw std::runtime_error("Shared memory should have been unlinked");
    }
}

TEST(TransportV3AdvancedTest, ConnectToNotReadyNode) {
    // 1. Create a fake node shared memory
    std::string node_id = "not_ready_node";
    std::string shm_name = "/librpc_node_not_ready";
    
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    ASSERT_GT(fd, 0);
    ftruncate(fd, sizeof(Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory));
    
    void* addr = mmap(nullptr, sizeof(Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto* shm = static_cast<Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory*>(addr);
    
    shm->header.magic.store(Nexus::rpc::SharedMemoryTransportV3::MAGIC);
    shm->header.ready.store(false); // Not ready!
    shm->header.owner_pid.store(getpid()); // Alive
    
    // 2. Register it
    SharedMemoryRegistry registry;
    registry.initialize();
    registry.registerNode(node_id, shm_name);
    
    // 3. Try to connect
    TransportV3TestHelper transport;
    Nexus::rpc::SharedMemoryTransportV3::Config config;
    transport.initialize("client_node", config);
    
    // Should fail because node is not ready
    ASSERT_FALSE(transport.test_connectToNode(node_id));
    
    // Cleanup
    registry.unregisterNode(node_id);
    munmap(addr, sizeof(Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory));
    close(fd);
    shm_unlink(shm_name.c_str());
}

TEST(TransportV3AdvancedTest, ConnectToDeadNode) {
    // 1. Create a fake node shared memory
    std::string node_id = "dead_node";
    std::string shm_name = "/librpc_node_dead";
    
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    ASSERT_GT(fd, 0);
    ftruncate(fd, sizeof(Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory));
    
    void* addr = mmap(nullptr, sizeof(Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto* shm = static_cast<Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory*>(addr);
    
    shm->header.magic.store(Nexus::rpc::SharedMemoryTransportV3::MAGIC);
    shm->header.ready.store(true);
    shm->header.owner_pid.store(999999); // Dead PID
    
    // 2. Register it
    SharedMemoryRegistry registry;
    registry.initialize();
    registry.registerNode(node_id, shm_name);
    
    // 3. Try to connect
    TransportV3TestHelper transport;
    Nexus::rpc::SharedMemoryTransportV3::Config config;
    transport.initialize("client_node_2", config);
    
    // Should fail because owner is dead
    ASSERT_FALSE(transport.test_connectToNode(node_id));
    
    // Cleanup
    registry.unregisterNode(node_id);
    munmap(addr, sizeof(Nexus::rpc::SharedMemoryTransportV3::NodeSharedMemory));
    close(fd);
    shm_unlink(shm_name.c_str());
}

#include "nexus/registry/GlobalRegistry.h"

// Helper for cleanup
struct TransportV3ModeCleanup {
    TransportV3ModeCleanup() { clear(); }
    ~TransportV3ModeCleanup() { clear(); }
    
    void clear() {
        GlobalRegistry::instance().clearServices();
    }
};

TEST(TransportV3AdvancedTest, QueueLimitReached) {
    TransportV3ModeCleanup cleanup;
    
    // 1. Create Receiver with max_inbound_queues = 1
    SharedMemoryTransportV3 receiver;
    SharedMemoryTransportV3::Config config;
    config.max_inbound_queues = 1;
    ASSERT_TRUE(receiver.initialize("limit_recv", config));
    
    // 2. Connect Sender 1 (Should succeed)
    SharedMemoryTransportV3 sender1;
    ASSERT_TRUE(sender1.initialize("sender1", config));
    // Send empty message to trigger connection
    std::vector<uint8_t> data = {1};
    ASSERT_TRUE(sender1.send("limit_recv", data.data(), data.size()));
    
    // 3. Connect Sender 2 (Should fail)
    SharedMemoryTransportV3 sender2;
    ASSERT_TRUE(sender2.initialize("sender2", config));
    
    // send should fail because connection fails due to queue limit
    ASSERT_FALSE(sender2.send("limit_recv", data.data(), data.size()));
}

TEST(TransportV3AdvancedTest, DisconnectNonExistent) {
    TransportV3TestHelper transport;
    Nexus::rpc::SharedMemoryTransportV3::Config config;
    transport.initialize("disconnect_test", config);
    
    // Should not crash
    transport.disconnectFromNode("non_existent");
}
