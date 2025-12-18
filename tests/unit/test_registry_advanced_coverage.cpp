#include "simple_test.h"
#include "nexus/registry/SharedMemoryRegistry.h"
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

using namespace Nexus::rpc;

// Helper macros for missing assertions
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))

// RAII helper for cleanup
class RegistryCleanup {
public:
    RegistryCleanup() {
        SharedMemoryRegistry::forceRemoveRegistry();
    }
    ~RegistryCleanup() {
        SharedMemoryRegistry::forceRemoveRegistry();
    }
};

TEST(RegistryAdvancedTest, ForceRemoveRegistry) {
    RegistryCleanup cleanup;
    
    // 1. Create a registry
    {
        SharedMemoryRegistry registry;
        ASSERT_TRUE(registry.initialize());
        ASSERT_TRUE(registry.registerNode("test_node", "/test_shm"));
    }

    // 2. Force remove it
    ASSERT_TRUE(SharedMemoryRegistry::forceRemoveRegistry());

    // 3. Verify it's gone (shm_open should fail with ENOENT)
    int fd = shm_open("/librpc_registry", O_RDWR, 0666);
    if (fd >= 0) {
        close(fd);
        ASSERT_TRUE(false); // Should not exist
    }
}

TEST(RegistryAdvancedTest, CleanupStaleNodes_DeadProcess) {
    RegistryCleanup cleanup;

    // 1. Initialize registry in parent
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());

    // 2. Fork a child process to register a node and then die
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        SharedMemoryRegistry child_registry;
        if (child_registry.initialize()) {
            child_registry.registerNode("child_node", "/child_shm");
            // Exit without cleanup (simulate crash)
            _exit(0);
        }
        _exit(1);
    }

    ASSERT_GT(pid, 0);

    // 3. Wait for child to die
    int status;
    waitpid(pid, &status, 0);

    // 4. Run cleanup
    // cleanupStaleNodes checks kill(pid, 0)
    int cleaned = registry.cleanupStaleNodes(0); // 0 timeout
    
    // Should have cleaned 1 node
    ASSERT_EQ(cleaned, 1);

    // 5. Verify node is gone
    NodeInfo info;
    ASSERT_FALSE(registry.findNode("child_node", info));
}

TEST(RegistryAdvancedTest, RegistryFull) {
    RegistryCleanup cleanup;
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());

    // Fill registry
    for (int i = 0; i < SharedMemoryRegistry::MAX_REGISTRY_ENTRIES; ++i) {
        std::string id = "node_" + std::to_string(i);
        ASSERT_TRUE(registry.registerNode(id, "/shm_" + id));
    }

    // Try one more
    ASSERT_FALSE(registry.registerNode("overflow", "/overflow"));
}

TEST(RegistryAdvancedTest, InvalidInputs) {
    RegistryCleanup cleanup;
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());

    // Empty ID
    ASSERT_FALSE(registry.registerNode("", "/shm"));
    
    // Empty SHM
    ASSERT_FALSE(registry.registerNode("id", ""));

    // Too long ID
    std::string long_id(SharedMemoryRegistry::NODE_ID_SIZE + 1, 'a');
    ASSERT_FALSE(registry.registerNode(long_id, "/shm"));

    // Too long SHM
    std::string long_shm(SharedMemoryRegistry::SHM_NAME_SIZE + 1, 'b');
    ASSERT_FALSE(registry.registerNode("id", long_shm));
}

TEST(RegistryAdvancedTest, DoubleInitialization) {
    RegistryCleanup cleanup;
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    // Second init should return true immediately
    ASSERT_TRUE(registry.initialize());
}

TEST(RegistryAdvancedTest, UnregisterNonExistent) {
    RegistryCleanup cleanup;
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    ASSERT_FALSE(registry.unregisterNode("non_existent"));
}

TEST(RegistryAdvancedTest, UpdateHeartbeatNonExistent) {
    RegistryCleanup cleanup;
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    ASSERT_FALSE(registry.updateHeartbeat("non_existent"));
}

TEST(RegistryAdvancedTest, AmICleanupMaster) {
    RegistryCleanup cleanup;
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());
    
    ASSERT_TRUE(registry.amICleanupMaster());
}

TEST(RegistryAdvancedTest, CleanupOrphanedRegistry_InUse) {
    RegistryCleanup cleanup;
    // 1. Create registry and keep it open
    SharedMemoryRegistry registry;
    ASSERT_TRUE(registry.initialize());

    // 2. Try to cleanup from another "process" (same process here, but logic checks lock)
    // Since we hold the lock only during operations, cleanupOrphanedRegistry might succeed 
    // if it can acquire the lock.
    // But cleanupOrphanedRegistry checks if there are alive processes.
    // We are alive.
    
    // It should return true (success) but NOT remove the registry
    ASSERT_TRUE(SharedMemoryRegistry::cleanupOrphanedRegistry());
    
    // Verify registry still exists
    int fd = shm_open("/librpc_registry", O_RDWR, 0666);
    ASSERT_GE(fd, 0);
    close(fd);
}
