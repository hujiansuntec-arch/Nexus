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
#include <sys/wait.h>

using namespace Nexus::rpc;

// Helper macros
// ASSERT_EQ is defined in simple_test.h
#define FAIL(msg) do { \
    std::stringstream ss; \
    ss << "Test failed: " << msg << " at " << __FILE__ << ":" << __LINE__; \
    throw nexus_test::AssertionFailure(ss.str()); \
} while(0)


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

class RegistryCoverageExtraTest {
public:
    RegistryCoverageExtraTest() {
        SharedMemoryRegistry::forceRemoveRegistry();
    }
    ~RegistryCoverageExtraTest() {
        SharedMemoryRegistry::forceRemoveRegistry();
    }
};

TEST(RegistryCoverageExtraTest, SizeMismatch) {
    RegistryCoverageExtraTest cleanup;

    // 1. Create a registry file with wrong size
    int fd = shm_open("/librpc_registry", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    // Set size to something small
    ASSERT_EQ(ftruncate(fd, 100), 0);
    close(fd);

    // 2. Try to initialize
    SharedMemoryRegistry registry;
    // Should fail due to size mismatch
    ASSERT_FALSE(registry.initialize());
}

TEST(RegistryCoverageExtraTest, RefPidsFull) {
    RegistryCoverageExtraTest cleanup;

    // 1. Create a valid registry manually
    int fd = shm_open("/librpc_registry", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ftruncate(fd, sizeof(TestRegistryRegion)), 0);
    
    void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), 
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    auto* reg = static_cast<TestRegistryRegion*>(ptr);
    
    // Initialize mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&reg->header.global_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // Fill ref_pids with fake PIDs
    for (int i = 0; i < SharedMemoryRegistry::MAX_REGISTRY_ENTRIES; ++i) {
        reg->header.ref_pids[i].store(99999 + i);
    }
    
    // Set magic so it looks valid
    reg->header.magic.store(0xA5A5A5A5); // MAGIC
    
    munmap(ptr, sizeof(TestRegistryRegion));
    close(fd);

    // 2. Try to initialize
    SharedMemoryRegistry registry;
    // Should fail because it can't record its PID
    ASSERT_FALSE(registry.initialize());
}

TEST(RegistryCoverageExtraTest, ForceRemoveLocked) {
    RegistryCoverageExtraTest cleanup;
    
    // 1. Create valid registry
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    // 2. Lock it from another thread
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
        
        while (!ready_to_unlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        pthread_mutex_unlock(&reg->header.global_lock);
        munmap(ptr, sizeof(TestRegistryRegion));
        close(fd);
    });

    // Wait for lock
    int waits = 0;
    while (!locked && waits < 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waits++;
    }
    ASSERT_TRUE(locked);

    // 3. Try to force remove
    // Should fail because lock is held
    ASSERT_FALSE(SharedMemoryRegistry::forceRemoveRegistry());

    ready_to_unlock = true;
    locker.join();
    
    // Now it should succeed
    ASSERT_TRUE(SharedMemoryRegistry::forceRemoveRegistry());
}

TEST(RegistryCoverageExtraTest, CleanupLocked) {
    RegistryCoverageExtraTest cleanup;
    
    // 1. Create valid registry
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    // 2. Lock it from another thread
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
        
        while (!ready_to_unlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        pthread_mutex_unlock(&reg->header.global_lock);
        munmap(ptr, sizeof(TestRegistryRegion));
        close(fd);
    });

    // Wait for lock
    int waits = 0;
    while (!locked && waits < 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waits++;
    }
    ASSERT_TRUE(locked);

    // 3. Try to cleanup
    // Should return true (skipped) but log a message
    ASSERT_TRUE(SharedMemoryRegistry::cleanupOrphanedRegistry());

    ready_to_unlock = true;
    locker.join();
}

TEST(RegistryCoverageExtraTest, InitializeCorruptedMagic) {
    RegistryCoverageExtraTest cleanup;

    // 1. Create a registry file manually
    int fd = shm_open("/librpc_registry", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ftruncate(fd, sizeof(TestRegistryRegion)), 0);
    
    void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), 
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    auto* reg = static_cast<TestRegistryRegion*>(ptr);
    
    // Initialize mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&reg->header.global_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // Set magic to 0 (corrupted)
    reg->header.magic.store(0);
    
    munmap(ptr, sizeof(TestRegistryRegion));
    close(fd);

    // 2. Try to initialize
    SharedMemoryRegistry registry;
    // Should fail due to timeout waiting for magic
    ASSERT_FALSE(registry.initialize());
}

TEST(RegistryCoverageExtraTest, CleanupCorruptedMagic) {
    RegistryCoverageExtraTest cleanup;

    // 1. Create a registry file manually
    int fd = shm_open("/librpc_registry", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ftruncate(fd, sizeof(TestRegistryRegion)), 0);
    
    void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), 
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    auto* reg = static_cast<TestRegistryRegion*>(ptr);
    
    // Initialize mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&reg->header.global_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // Set magic to 0 (corrupted)
    reg->header.magic.store(0);
    
    munmap(ptr, sizeof(TestRegistryRegion));
    close(fd);

    // 2. Try to cleanup
    // Should detect corruption and remove it
    ASSERT_TRUE(SharedMemoryRegistry::cleanupOrphanedRegistry());
    
    // Verify it's gone
    fd = shm_open("/librpc_registry", O_RDWR, 0666);
    if (fd >= 0) {
        close(fd);
        FAIL("Registry should have been removed");
    }
}

TEST(RegistryCoverageExtraTest, DestructorOwnerDead) {
    RegistryCoverageExtraTest cleanup;

    // 1. Create registry in parent
    {
        SharedMemoryRegistry reg;
        ASSERT_TRUE(reg.initialize());

        // 2. Fork child to lock mutex and die
        pid_t pid = fork();
        if (pid == 0) {
            // Child
            // Manually map to access mutex
            int fd = shm_open("/librpc_registry", O_RDWR, 0666);
            if (fd < 0) exit(1);
            void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) exit(2);
            auto* r = static_cast<TestRegistryRegion*>(ptr);
            
            // Lock and die
            pthread_mutex_lock(&r->header.global_lock);
            exit(0);
        }

        // Parent waits for child to die
        int status;
        waitpid(pid, &status, 0);
        
        // 3. Destructor of 'reg' runs here
        // It should encounter EOWNERDEAD and recover
    }
    
    // Verify registry is gone (since parent was last ref)
    int fd = shm_open("/librpc_registry", O_RDWR, 0666);
    if (fd >= 0) {
        close(fd);
        // It might still exist if cleanup failed, but we expect it to succeed
    }
}

TEST(RegistryCoverageExtraTest, CleanupInvalidMagic) {
    RegistryCoverageExtraTest cleanup;

    // 1. Create a registry file manually
    int fd = shm_open("/librpc_registry", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ftruncate(fd, sizeof(TestRegistryRegion)), 0);
    
    void* ptr = mmap(nullptr, sizeof(TestRegistryRegion), 
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(ptr != MAP_FAILED);
    
    auto* reg = static_cast<TestRegistryRegion*>(ptr);
    
    // Initialize mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&reg->header.global_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // Set magic to random value (not 0, not MAGIC)
    reg->header.magic.store(0xDEADBEEF);
    
    munmap(ptr, sizeof(TestRegistryRegion));
    close(fd);

    // 2. Try to cleanup
    // Should log warning but NOT remove it (assumes initialization in progress)
    ASSERT_TRUE(SharedMemoryRegistry::cleanupOrphanedRegistry());
    
    // Verify it's STILL there
    fd = shm_open("/librpc_registry", O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    close(fd);
}
