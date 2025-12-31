#include "simple_test.h"
#include "nexus/transport/LargeDataChannel.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <iostream>

using namespace Nexus::rpc;

// Helper to create a fake orphaned channel
void create_fake_channel(const std::string& name, int32_t writer_pid, int32_t ref_count) {
    std::string shm_name = "nexus_ld_" + name;
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return;
    }

    size_t size = sizeof(RingBufferControl) + 4096; // Min valid size
    ftruncate(fd, size);

    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr != MAP_FAILED) {
        // Initialize control structure
        RingBufferControl* control = new (addr) RingBufferControl();
        control->writer_pid.store(writer_pid);
        control->ref_count.store(ref_count);
        control->capacity = 4096;
        control->max_block_size = 1024;
        
        munmap(addr, size);
    } else {
        perror("mmap");
    }
    close(fd);
}

TEST(LargeDataCleanup, CleanupZeroRefCount) {
    std::string name = "channel_test_cleanup_zero_ref";
    std::string shm_name = "nexus_ld_" + name;
    
    // Ensure clean state
    shm_unlink(shm_name.c_str());
    
    // Create fake channel with ref_count = 0
    create_fake_channel(name, getpid(), 0);
    
    // Verify file exists
    int fd = shm_open(shm_name.c_str(), O_RDONLY, 0666);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    
    // Run cleanup
    size_t cleaned = LargeDataChannel::cleanupOrphanedChannels(0);
    ASSERT_TRUE(cleaned > 0);
    
    // Verify file is gone
    fd = shm_open(shm_name.c_str(), O_RDONLY, 0666);
    ASSERT_TRUE(fd < 0);
}

TEST(LargeDataCleanup, CleanupDeadProcess) {
    std::string name = "channel_test_cleanup_dead_proc";
    std::string shm_name = "nexus_ld_" + name;
    
    // Ensure clean state
    shm_unlink(shm_name.c_str());
    
    // Create fake channel with dead PID and ref_count > 0
    // PID 999999 is unlikely to exist
    create_fake_channel(name, 999999, 1);
    
    // Verify file exists
    int fd = shm_open(shm_name.c_str(), O_RDONLY, 0666);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    
    // Run cleanup
    size_t cleaned = LargeDataChannel::cleanupOrphanedChannels(0);
    ASSERT_TRUE(cleaned > 0);
    
    // Verify file is gone
    fd = shm_open(shm_name.c_str(), O_RDONLY, 0666);
    ASSERT_TRUE(fd < 0);
}

TEST(LargeDataCleanup, KeepAliveProcess) {
    std::string name = "channel_test_keep_alive";
    std::string shm_name = "nexus_ld_" + name;
    
    // Ensure clean state
    shm_unlink(shm_name.c_str());
    
    // Create fake channel with current PID (alive) and ref_count > 0
    create_fake_channel(name, getpid(), 1);
    
    // Verify file exists
    int fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    ASSERT_TRUE(fd >= 0);
    
    // Lock it to simulate active usage
    flock(fd, LOCK_SH | LOCK_NB);
    
    // Run cleanup
    size_t cleaned = LargeDataChannel::cleanupOrphanedChannels(0);
    ASSERT_EQ(cleaned, 0);
    
    // Verify file still exists
    struct stat st;
    ASSERT_EQ(fstat(fd, &st), 0);
    
    close(fd); // Release lock
    
    // Cleanup manually
    shm_unlink(shm_name.c_str());
}
