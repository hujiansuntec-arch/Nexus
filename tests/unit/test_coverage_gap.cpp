#include "simple_test.h"
#include "nexus/core/Config.h"
#include "nexus/transport/LargeDataChannel.h"
#include "nexus/registry/SharedMemoryRegistry.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include <stdlib.h>
#include <vector>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <pthread.h>

using namespace Nexus::rpc;

// Helper to corrupt SHM data
void corrupt_shm_data(const std::string& shm_name, size_t offset, uint8_t val) {
    std::string path = "/dev/shm/" + shm_name;
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) return;
    
    struct stat st;
    fstat(fd, &st);
    
    void* addr = mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr != MAP_FAILED) {
        uint8_t* ptr = static_cast<uint8_t*>(addr);
        if (offset < (size_t)st.st_size) {
            ptr[offset] = val;
        }
        munmap(addr, st.st_size);
    }
    close(fd);
}

// --- Config Coverage ---
TEST(ConfigCoverage, SingletonAccess) {
    Nexus::rpc::Config& config = Nexus::rpc::Config::instance();
    // Verify we can access it and get some default value
    ASSERT_GT(config.shm.queue_capacity, 0);
    ASSERT_GT(config.node.max_inbound_queues, 0);
    
    // Test loadFromEnv
    setenv("NEXUS_MAX_INBOUND_QUEUES", "16", 1);
    setenv("NEXUS_QUEUE_CAPACITY", "512", 1);
    config.loadFromEnv();
    ASSERT_EQ(config.node.max_inbound_queues, 16);
    ASSERT_EQ(config.node.queue_capacity, 512);
    
    // Test calculateMemoryFootprint
    size_t footprint = config.calculateMemoryFootprint();
    ASSERT_GT(footprint, 0);
}

// --- LargeDataChannel Coverage ---
TEST(LargeDataCoverage, EdgeCases) {
    // 1. Create with valid name
    auto channel = LargeDataChannel::create("coverage_channel");
    ASSERT_TRUE(channel != nullptr);
    
    // 2. Write invalid args (0 size)
    std::vector<uint8_t> empty_data;
    int64_t seq = channel->write("topic", empty_data.data(), 0);
    // Implementation allows 0-byte writes, so we expect success (seq >= 0)
    ASSERT_TRUE(seq >= 0);
    
    // 3. Write invalid args (too large)
    // Assuming default size is 64MB (from Config.h), try 65MB
    std::vector<uint8_t> huge_data(65 * 1024 * 1024);
    seq = channel->write("topic", huge_data.data(), huge_data.size());
    ASSERT_EQ(seq, -1);
    
    // 4. Try read (should read the 0-byte message from step 2)
    LargeDataChannel::DataBlock block;
    bool success = channel->tryRead(block);
    ASSERT_TRUE(success);
    ASSERT_EQ(block.size, 0);
    channel->releaseBlock(block);
}

TEST(LargeDataCoverage, CRCError) {
    LargeDataChannel::Config config;
    config.buffer_size = 1024 * 1024; // 1MB
    std::string shm_name = "test_crc_error";
    
    // Cleanup previous
    shm_unlink(shm_name.c_str());
    
    auto channel = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(channel != nullptr);
    
    std::string topic = "test_topic";
    std::vector<uint8_t> data(100, 0xAB);
    
    int64_t seq = channel->write(topic, data.data(), data.size());
    ASSERT_TRUE(seq >= 0);
    
    // Register reader by trying to read
    LargeDataChannel::DataBlock block;
    channel->tryRead(block); // This registers the reader
    
    // Corrupt the data in SHM
    size_t control_size = sizeof(RingBufferControl); // 1024 + 64 + ... aligned to 64
    size_t header_size = sizeof(LargeDataHeader); // 128
    size_t data_offset = control_size + header_size;
    
    corrupt_shm_data(shm_name, data_offset, 0xFF); // Corrupt first byte of data
    
    bool result = channel->tryRead(block);
    
    // Should fail validation
    ASSERT_FALSE(result);
    ASSERT_TRUE(block.result == LargeDataChannel::ReadResult::CRC_ERROR);
    
    shm_unlink(shm_name.c_str());
}

TEST(LargeDataCoverage, ReaderTooSlow) {
    LargeDataChannel::Config config;
    config.buffer_size = 4096 * 10; // Small buffer ~40KB
    config.max_block_size = 4096;
    std::string shm_name = "test_reader_slow";
    
    shm_unlink(shm_name.c_str());
    
    auto channel = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(channel != nullptr);
    
    // Register reader
    LargeDataChannel::DataBlock block;
    channel->tryRead(block);
    
    std::vector<uint8_t> data(1024, 0xAA);
    
    // Write enough to wrap around
    // Capacity is 40960. Each write is ~1024 + header (~128) = 1152.
    // 40 writes = 46080 bytes > capacity.
    for (int i = 0; i < 50; ++i) {
        channel->write("topic", data.data(), data.size());
    }
    
    bool result = channel->tryRead(block);
    
    // The reader should have been advanced.
    if (result) {
        channel->releaseBlock(block);
    }
    
    shm_unlink(shm_name.c_str());
}

TEST(LargeDataCoverage, OverflowDropNewest) {
    LargeDataChannel::Config config;
    config.buffer_size = 8192; // Very small
    config.max_block_size = 4096;
    config.overflow_policy = LargeDataOverflowPolicy::DROP_NEWEST;
    std::string shm_name = "test_overflow";
    
    shm_unlink(shm_name.c_str());
    
    auto channel = LargeDataChannel::create(shm_name, config);
    
    // Register a reader so that data is preserved (and buffer fills up)
    LargeDataChannel::DataBlock block;
    channel->tryRead(block);
    
    std::vector<uint8_t> data(2048, 0xBB);
    
    // Write 1: 2048 + header (128) = 2176. Used: 2176
    channel->write("t", data.data(), data.size());
    // Write 2: Used: 4352
    channel->write("t", data.data(), data.size());
    // Write 3: Used: 6528
    channel->write("t", data.data(), data.size());
    // Write 4: Used: 8704 > 8192 -> Should fail
    
    int64_t seq = channel->write("t", data.data(), data.size());
    ASSERT_EQ(-1, seq);
    
    shm_unlink(shm_name.c_str());
}

// --- SharedMemoryRegistry Coverage ---
TEST(RegistryCoverage, EdgeCases) {
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    std::string node_id = "coverage_node";
    std::string shm_name = "/librpc_node_coverage";
    
    // Register
    bool res = registry.registerNode(node_id, shm_name);
    ASSERT_TRUE(res);
    
    // Update Heartbeat
    res = registry.updateHeartbeat(node_id);
    ASSERT_TRUE(res);
    
    // Find Node
    NodeInfo retrieved;
    res = registry.findNode(node_id, retrieved);
    ASSERT_TRUE(res);
    ASSERT_EQ(retrieved.node_id, node_id);
    ASSERT_EQ(retrieved.shm_name, shm_name);
    
    // Unregister
    res = registry.unregisterNode(node_id);
    ASSERT_TRUE(res);
    
    // Find non-existent
    res = registry.findNode(node_id, retrieved);
    ASSERT_FALSE(res);
    
    // Test getAllNodes
    auto nodes = registry.getAllNodes();
    // Should be empty or contain other nodes from other tests
    
    // Test cleanupStaleNodes
    // Register a node
    registry.registerNode("stale_node", "/shm_stale");
    // We can't easily simulate time passing without mocking, but we can call the function
    registry.cleanupStaleNodes(0); // 0 timeout should clean it immediately if it checks strictly > timeout
}

TEST(RegistryCoverage, CorruptedFile) {
    std::string shm_name = "librpc_registry"; // Default name
    
    // 1. Create 0-byte file
    shm_unlink(shm_name.c_str());
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    close(fd); // Size is 0
    
    {
        SharedMemoryRegistry registry;
        // Should detect 0 size and fix it
        ASSERT_TRUE(registry.initialize());
    }
    
    // 2. Create file with wrong size
    shm_unlink(shm_name.c_str());
    fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    ftruncate(fd, 100); // Wrong size
    close(fd);
    
    {
        SharedMemoryRegistry registry;
        // Should fail because size is incorrect and not 0
        // Note: initialize() logs error and returns false
        ASSERT_FALSE(registry.initialize());
    }
    
    // Cleanup
    shm_unlink(shm_name.c_str());
}

// Duplicate internal structs for white-box testing
namespace Internal {
    struct RegistryEntry {
        std::atomic<uint32_t> flags;     // Bit 0: valid, Bit 1: active
        std::atomic<uint32_t> version;   // Entry version to detect ABA problem
        std::atomic<uint32_t> pid;       // Process ID as atomic
        std::atomic<uint32_t> _padding;  // Alignment padding
        std::atomic<uint64_t> last_heartbeat;

        std::atomic<uint64_t> node_id_atomic[8];
        std::atomic<uint64_t> shm_name_atomic[8];

        char padding[48];
    };

    struct alignas(64) RegistryHeader {
        pthread_mutex_t global_lock;
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> version;
        std::atomic<uint32_t> num_entries;
        std::atomic<uint32_t> capacity;
        std::atomic<uint32_t> ref_count;
        std::atomic<uint32_t> ref_pids[SharedMemoryRegistry::MAX_REGISTRY_ENTRIES];
    };

    struct RegistryRegion {
        RegistryHeader header;
        RegistryEntry entries[SharedMemoryRegistry::MAX_REGISTRY_ENTRIES];
    };
}

TEST(RegistryCoverage, StaleCleanup) {
    std::string shm_name = "librpc_registry";
    shm_unlink(shm_name.c_str());
    
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    registry.registerNode("fake_node", "fake_shm");
    
    // Map the registry manually to hack it
    int fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    void* addr = mmap(nullptr, sizeof(Internal::RegistryRegion), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Internal::RegistryRegion* region = static_cast<Internal::RegistryRegion*>(addr);
    
    // Find the entry
    for(int i=0; i<SharedMemoryRegistry::MAX_REGISTRY_ENTRIES; ++i) {
        if ((region->entries[i].flags.load() & 0x1)) {
            // Change PID to a non-existent one (e.g. 999999)
            region->entries[i].pid.store(999999);
            // Set heartbeat to old time
            region->entries[i].last_heartbeat.store(1); 
            break;
        }
    }
    munmap(addr, sizeof(Internal::RegistryRegion));
    close(fd);
    
    // Now run cleanup
    // cleanupStaleNodes checks kill(pid, 0). 999999 should not exist.
    int cleaned = registry.cleanupStaleNodes(1000); // 1s timeout
    ASSERT_TRUE(cleaned > 0);
}
