#include "simple_test.h"
#include "nexus/registry/SharedMemoryRegistry.h"
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>

using namespace Nexus::rpc;

// Helper macros
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

// Mirror of private structs for testing layout
struct TestRegistryEntry {
    std::atomic<uint32_t> flags;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> pid;
    std::atomic<uint32_t> _padding;
    std::atomic<uint64_t> last_heartbeat;
    std::atomic<uint64_t> node_id_atomic[8];
    std::atomic<uint64_t> shm_name_atomic[8];
    char padding[48];
};

struct alignas(64) TestRegistryHeader {
    pthread_mutex_t global_lock;
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> num_entries;
    std::atomic<uint32_t> capacity;
    std::atomic<uint32_t> ref_count;
    std::atomic<uint32_t> ref_pids[SharedMemoryRegistry::MAX_REGISTRY_ENTRIES];
};

struct TestRegistryRegion {
    TestRegistryHeader header;
    TestRegistryEntry entries[SharedMemoryRegistry::MAX_REGISTRY_ENTRIES];
};

class RegistryCoverageTest {
public:
    RegistryCoverageTest() {
        SharedMemoryRegistry::forceRemoveRegistry();
    }
    ~RegistryCoverageTest() {
        SharedMemoryRegistry::forceRemoveRegistry();
    }
};

TEST(RegistryCoverageTest, CorruptedRegistry) {
    RegistryCoverageTest cleanup;

    // 1. Manually create a corrupted registry (magic = 0)
    int fd = shm_open("/librpc_registry", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ftruncate(fd, sizeof(TestRegistryRegion)), 0);
    
    // Map and zero out
    void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), 
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    std::memset(ptr, 0, sizeof(TestRegistryRegion));
    
    munmap(ptr, sizeof(TestRegistryRegion));
    close(fd);

    // 2. Try to initialize
    SharedMemoryRegistry registry;
    // It should fail because magic is 0 and it times out waiting for it to become MAGIC
    ASSERT_FALSE(registry.initialize());
}

TEST(RegistryCoverageTest, LockContention) {
    RegistryCoverageTest cleanup;
    
    // 1. Initialize properly first and keep it alive
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    // Register a node so we can try to update its heartbeat
    ASSERT_TRUE(registry.registerNode("test_node", "/test_shm"));

    // 2. Manually lock the registry from another thread
    std::atomic<bool> locked{false};
    std::atomic<bool> ready_to_unlock{false};
    
    std::thread locker([&]() {
        int fd = shm_open("/librpc_registry", O_RDWR, 0666);
        if (fd < 0) return;
        
        void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), 
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) { close(fd); return; }
        
        auto* reg = static_cast<TestRegistryRegion*>(ptr);
        
        pthread_mutex_lock(&reg->header.global_lock);
        locked = true;
        
        // Hold lock until told to release
        while (!ready_to_unlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        pthread_mutex_unlock(&reg->header.global_lock);
        munmap(ptr, sizeof(TestRegistryRegion));
        close(fd);
    });

    // Wait for locker to acquire lock
    int waits = 0;
    while (!locked && waits < 200) { // Wait up to 2s
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waits++;
    }
    ASSERT_TRUE(locked);

    // 3. Try to update heartbeat (should fail due to lock timeout of 1s)
    // Note: registerNode has 5s timeout, updateHeartbeat has 1s.
    bool result = registry.updateHeartbeat("test_node");
    
    // Should fail because lock is held by other thread
    ASSERT_FALSE(result);

    ready_to_unlock = true;
    locker.join();
}

TEST(RegistryCoverageTest, ConcurrentRegistration) {
    RegistryCoverageTest cleanup;
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());

    const int NUM_THREADS = 10;
    const int NODES_PER_THREAD = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < NODES_PER_THREAD; ++j) {
                std::string id = "node_" + std::to_string(i) + "_" + std::to_string(j);
                if (registry.registerNode(id, "/shm_" + id)) {
                    success_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(success_count, NUM_THREADS * NODES_PER_THREAD);
    ASSERT_EQ(registry.getActiveNodeCount(), NUM_THREADS * NODES_PER_THREAD);
}

TEST(RegistryCoverageTest, CleanupOrphanedRegistry_Corrupted) {
    RegistryCoverageTest cleanup;

    // 1. Create corrupted registry (magic = 0)
    int fd = shm_open("/librpc_registry", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ftruncate(fd, sizeof(TestRegistryRegion)), 0);
    void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    std::memset(ptr, 0, sizeof(TestRegistryRegion));
    
    // Initialize mutex so we can lock it (cleanupOrphanedRegistry tries to lock)
    auto* reg = static_cast<TestRegistryRegion*>(ptr);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&reg->header.global_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    
    munmap(ptr, sizeof(TestRegistryRegion));
    close(fd);

    // 2. Run cleanup
    // It should detect magic=0 and remove it
    ASSERT_TRUE(SharedMemoryRegistry::cleanupOrphanedRegistry());

    // 3. Verify it's gone
    fd = shm_open("/librpc_registry", O_RDWR, 0666);
    if (fd >= 0) {
        close(fd);
        ASSERT_TRUE(false); // Should be removed
    }
}
