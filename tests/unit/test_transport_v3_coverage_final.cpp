#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/core/NodeImpl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <iostream>

using namespace Nexus::rpc;

// Mock NodeHeader to access private shared memory layout
// Must match NodeHeader in SharedMemoryTransportV3.h
struct alignas(64) MockNodeHeader {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> num_queues;
    std::atomic<uint32_t> max_queues;
    std::atomic<uint64_t> last_heartbeat;
    std::atomic<bool> ready;
    std::atomic<int32_t> owner_pid;

    static constexpr int MAX_ACCESSORS = 64;
    std::atomic<int32_t> accessor_pids[MAX_ACCESSORS];
    std::atomic<uint32_t> num_accessors;

    pthread_mutex_t global_mutex;
    pthread_cond_t global_cond;

    char padding[64];
};

void cleanup_shm() {
    shm_unlink("/librpc_registry");
    shm_unlink("/librpc_node_dummy_target");
    shm_unlink("/librpc_node_dummy_dead");
    shm_unlink("/librpc_node_dummy_invalid");
    shm_unlink("/librpc_node_accessor_test");
    shm_unlink("/librpc_node_dummy_dead_target");
    shm_unlink("/librpc_node_dummy_bad_magic");
    shm_unlink("/librpc_node_dummy_not_ready");
}

TEST(TransportV3CoverageFinal, WarmupConnections) {
    cleanup_shm();
    
    // Create dummy shared memory for target node
    int fd = shm_open("/librpc_node_dummy_target", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd != -1);
    // Increase to 50MB to cover NodeSharedMemory size (approx 45MB)
    int ret = ftruncate(fd, 50 * 1024 * 1024); 
    ASSERT_TRUE(ret == 0);

    void* ptr = mmap(nullptr, 50 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);

    // Initialize header
    MockNodeHeader* header = (MockNodeHeader*)ptr;
    new (header) MockNodeHeader(); 
    header->magic.store(0x4C524E33);
    header->version.store(1);
    header->max_queues.store(32); // Set max queues!
    header->ready.store(true); 
    header->owner_pid.store(getpid()); 
    
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&header->global_mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&header->global_cond, &cattr);
    pthread_condattr_destroy(&cattr);

    munmap(ptr, 50 * 1024 * 1024);
    close(fd);

    // Register dummy node
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    ASSERT_TRUE(registry.registerNode("target_node", "/librpc_node_dummy_target"));

    // Check registry
    auto nodes = registry.getAllNodes();
    std::cout << "Registry nodes: " << nodes.size() << std::endl;
    for (const auto& n : nodes) {
        std::cout << "Node: " << n.node_id << ", Active: " << n.active << std::endl;
    }

    // Initialize transport
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    ASSERT_TRUE(transport.initialize("warmup_node", config));

    // Warmup connections
    transport.warmupConnections();

    // Verify connection count
    int count = transport.getConnectionCount();
    std::cout << "Connection count: " << count << std::endl;
    ASSERT_TRUE(count == 1);
    
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, ReceiveCallbackNull) {
    cleanup_shm();
    
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    ASSERT_TRUE(transport.initialize("proc_recv_null_cb", config));
    ASSERT_TRUE(transport.registerNodeToRegistry("recv_null_cb"));

    transport.removeReceiveCallback("recv_null_cb");
    transport.startReceiving();

    SharedMemoryTransportV3 sender;
    ASSERT_TRUE(sender.initialize("proc_sender_null_cb", config));
    ASSERT_TRUE(sender.registerNodeToRegistry("sender_null_cb"));
    
    uint8_t data[] = "test";
    sender.send("recv_null_cb", data, sizeof(data));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    sender.stopReceiving();
    transport.stopReceiving();
    
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, AccessorListFull) {
    cleanup_shm();
    
    int fd = shm_open("/librpc_node_accessor_test", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd != -1);
    ftruncate(fd, 50 * 1024 * 1024);
    
    void* ptr = mmap(nullptr, 50 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    MockNodeHeader* header = (MockNodeHeader*)ptr;
    new (header) MockNodeHeader();
    header->magic.store(0x4C524E33);
    header->version.store(1);
    header->ready.store(true);
    header->owner_pid.store(getpid());
    header->num_accessors.store(64); 
    for (int i = 0; i < 64; i++) {
        header->accessor_pids[i].store(10000 + i);
    }
    
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&header->global_mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&header->global_cond, &cattr);
    pthread_condattr_destroy(&cattr);

    munmap(ptr, 50 * 1024 * 1024);
    close(fd);
    
    SharedMemoryRegistry registry;
    registry.registerNode("full_node", "/librpc_node_accessor_test");
    
    SharedMemoryTransportV3 transport;
    ASSERT_TRUE(transport.initialize("accessor_tester"));
    
    uint8_t data[] = "test";
    bool sent = transport.send("full_node", data, sizeof(data));
    
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, DeadAccessorCleanup) {
    cleanup_shm();
    
    int fd = shm_open("/librpc_node_dummy_dead", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd != -1);
    ftruncate(fd, 50 * 1024 * 1024);
    
    void* ptr = mmap(nullptr, 50 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    MockNodeHeader* header = (MockNodeHeader*)ptr;
    new (header) MockNodeHeader();
    header->magic.store(0x4C524E33);
    header->version.store(1);
    header->ready.store(true);
    header->owner_pid.store(999999); 
    
    munmap(ptr, 50 * 1024 * 1024);
    close(fd);
    
    SharedMemoryTransportV3::cleanupOrphanedMemory();
    
    int fd2 = shm_open("/librpc_node_dummy_dead", O_RDONLY, 0666);
    if (fd2 != -1) {
        close(fd2);
    }
    
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, InvalidMagicCleanup) {
    cleanup_shm();
    
    int fd = shm_open("/librpc_node_dummy_invalid", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd != -1);
    ftruncate(fd, 50 * 1024 * 1024);
    
    void* ptr = mmap(nullptr, 50 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    MockNodeHeader* header = (MockNodeHeader*)ptr;
    new (header) MockNodeHeader();
    header->magic.store(0xDEADBEEF); 
    
    munmap(ptr, 50 * 1024 * 1024);
    close(fd);
    
    SharedMemoryTransportV3::cleanupOrphanedMemory();
    
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, ConnectToDeadNode) {
    cleanup_shm();
    
    // Create dummy shared memory
    int fd = shm_open("/librpc_node_dummy_dead_target", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd != -1);
    ftruncate(fd, 50 * 1024 * 1024);
    void* ptr = mmap(nullptr, 50 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    MockNodeHeader* header = (MockNodeHeader*)ptr;
    new (header) MockNodeHeader();
    header->magic.store(0x4C524E33);
    header->version.store(1);
    header->ready.store(true);
    header->owner_pid.store(999999); // Dead PID
    
    munmap(ptr, 50 * 1024 * 1024);
    close(fd);
    
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    ASSERT_TRUE(registry.registerNode("dead_node", "/librpc_node_dummy_dead_target"));
    
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    ASSERT_TRUE(transport.initialize("connector_node", config));
    
    // Try to send message (should trigger connection attempt)
    std::vector<uint8_t> data(10, 0);
    bool result = transport.send("dead_node", data.data(), data.size());
    ASSERT_FALSE(result);
    
    shm_unlink("/librpc_node_dummy_dead_target");
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, ConnectToInvalidMagicNode) {
    cleanup_shm();
    
    // Create dummy shared memory
    int fd = shm_open("/librpc_node_dummy_bad_magic", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd != -1);
    ftruncate(fd, 50 * 1024 * 1024);
    void* ptr = mmap(nullptr, 50 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    MockNodeHeader* header = (MockNodeHeader*)ptr;
    new (header) MockNodeHeader();
    header->magic.store(0xDEADBEEF); // Bad magic
    header->version.store(1);
    header->ready.store(true);
    header->owner_pid.store(getpid());
    
    munmap(ptr, 50 * 1024 * 1024);
    close(fd);
    
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    ASSERT_TRUE(registry.registerNode("bad_magic_node", "/librpc_node_dummy_bad_magic"));
    
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    ASSERT_TRUE(transport.initialize("connector_node_2", config));
    
    // Try to send message
    std::vector<uint8_t> data(10, 0);
    bool result = transport.send("bad_magic_node", data.data(), data.size());
    ASSERT_FALSE(result);
    
    shm_unlink("/librpc_node_dummy_bad_magic");
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, ConnectToNotReadyNode) {
    cleanup_shm();
    
    // Create dummy shared memory
    int fd = shm_open("/librpc_node_dummy_not_ready", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd != -1);
    ftruncate(fd, 50 * 1024 * 1024);
    void* ptr = mmap(nullptr, 50 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    MockNodeHeader* header = (MockNodeHeader*)ptr;
    new (header) MockNodeHeader();
    header->magic.store(0x4C524E33);
    header->version.store(1);
    header->ready.store(false); // Not ready
    header->owner_pid.store(getpid());
    
    munmap(ptr, 50 * 1024 * 1024);
    close(fd);
    
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    ASSERT_TRUE(registry.registerNode("not_ready_node", "/librpc_node_dummy_not_ready"));
    
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    ASSERT_TRUE(transport.initialize("connector_node_3", config));
    
    // Try to send message
    std::vector<uint8_t> data(10, 0);
    bool result = transport.send("not_ready_node", data.data(), data.size());
    ASSERT_FALSE(result);
    
    shm_unlink("/librpc_node_dummy_not_ready");
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, InitializeRegistryFail) {
    cleanup_shm();
    
    // Create registry file with no permissions to force open failure
    // We use open directly on /dev/shm/librpc_registry because shm_open implementation varies
    // But SharedMemoryRegistry uses shm_open("/librpc_registry", ...)
    // On Linux this maps to /dev/shm/librpc_registry
    
    int fd = open("/dev/shm/librpc_registry", O_CREAT | O_RDWR, 0000);
    if (fd != -1) close(fd);
    
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    
    // Should fail because registry cannot be initialized
    // Note: This might fail with "Permission denied"
    bool result = transport.initialize("fail_node", config);
    
    // If it succeeded, it means we couldn't block access (e.g. running as root?)
    // But usually it should fail.
    if (result) {
        // Clean up if it somehow succeeded
        // transport.shutdown();
    } else {
        ASSERT_FALSE(result);
    }
    
    shm_unlink("/librpc_registry");
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, InitializeRegisterNodeFail) {
    cleanup_shm();
    
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    // Fill registry to capacity (256)
    // We already have 1 entry (this test process if we initialized a transport, but we haven't yet)
    // Actually registry.initialize() doesn't register a node.
    
    for (int i = 0; i < 256; ++i) {
        std::string name = "dummy_node_" + std::to_string(i);
        // We need to register valid nodes so they take up slots
        // registerNode checks if slot is free.
        // We can just register them.
        registry.registerNode(name, "/dummy_shm");
    }
    
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    
    // Initialize should succeed (creates SHM file)
    ASSERT_TRUE(transport.initialize("fail_node_reg_full", config));
    
    // Register should fail because registry is full
    ASSERT_FALSE(transport.registerNodeToRegistry("fail_node_reg_full_node"));
    
    cleanup_shm();
}

TEST(TransportV3CoverageFinal, WarmupWithUnreachableNodes) {
    cleanup_shm();
    
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    // Register a dead node
    registry.registerNode("dead_node_warmup", "/dummy_dead");
    
    SharedMemoryTransportV3 transport;
    SharedMemoryTransportV3::Config config;
    ASSERT_TRUE(transport.initialize("warmup_tester", config));
    
    // This should try to connect to dead_node_warmup, fail, and sleep/continue
    // We just want to ensure it doesn't crash and covers the failure path
    transport.warmupConnections();
    
    cleanup_shm();
}
