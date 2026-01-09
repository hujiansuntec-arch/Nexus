#include "simple_test.h"
#include "nexus/transport/LargeDataChannel.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

using namespace Nexus::rpc;

TEST(LargeDataExtraCoverage, StatsAndSpace) {
    std::string channel_name = "test_stats";
    shm_unlink(channel_name.c_str());
    
    LargeDataChannel::Config config;
    config.buffer_size = 4096;
    auto writer = LargeDataChannel::create(channel_name, config);
    
    // Register as reader to track usage
    LargeDataChannel::DataBlock block;
    writer->tryRead(block);
    
    auto stats = writer->getStats();
    ASSERT_EQ(stats.total_writes, 0);
    ASSERT_EQ(stats.current_usage, 0);
    ASSERT_EQ(stats.capacity, 4096);
    
    ASSERT_TRUE(writer->canWrite(100));
    ASSERT_EQ(writer->getAvailableSpace(), 4096);
    
    std::vector<uint8_t> data(100, 0xAA);
    writer->write("topic", data.data(), data.size());
    
    stats = writer->getStats();
    ASSERT_EQ(stats.total_writes, 1);
    // Usage is header + data. Header is ~140 bytes.
    ASSERT_GT(stats.current_usage, 100); 
    ASSERT_LT(writer->getAvailableSpace(), 4096);
}

TEST(LargeDataExtraCoverage, OverflowPolicy) {
    std::string channel_name = "test_policy";
    shm_unlink(channel_name.c_str());
    
    LargeDataChannel::Config config;
    auto writer = LargeDataChannel::create(channel_name, config);
    
    writer->setOverflowPolicy(Nexus::rpc::LargeDataOverflowPolicy::DROP_NEWEST);
    writer->setOverflowPolicy(Nexus::rpc::LargeDataOverflowPolicy::BLOCK);
    
    writer->setOverflowCallback([](const std::string& channel, const std::string& topic, uint64_t seq, size_t dropped) {
        // callback
    });
}

TEST(LargeDataExtraCoverage, Cleanup_ProcessAlive) {
    std::string channel_name = "channel_alive_pid";
    shm_unlink(channel_name.c_str());
    
    // Manually create SHM to simulate another process
    int fd = shm_open(channel_name.c_str(), O_CREAT | O_RDWR, 0666);
    ftruncate(fd, sizeof(RingBufferControl) + 4096);
    void* addr = mmap(NULL, sizeof(RingBufferControl) + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    
    RingBufferControl* control = (RingBufferControl*)addr;
    control->ref_count = 1;
    control->writer_pid = 1; // Init process, usually alive
    control->writer_heartbeat = time(NULL); // Fresh heartbeat
    
    munmap(addr, sizeof(RingBufferControl) + 4096);
    close(fd);
    
    // Should NOT clean up because PID 1 is alive
    size_t cleaned = LargeDataChannel::cleanupOrphanedChannels(10);
    ASSERT_EQ(cleaned, 0);
    
    // Verify file exists
    fd = shm_open(channel_name.c_str(), O_RDONLY, 0666);
    ASSERT_NE(fd, -1);
    close(fd);
    shm_unlink(channel_name.c_str());
}

TEST(LargeDataExtraCoverage, Cleanup_ProcessDead) {
    std::string channel_name = "channel_dead_pid";
    shm_unlink(channel_name.c_str());
    
    int fd = shm_open(channel_name.c_str(), O_CREAT | O_RDWR, 0666);
    ftruncate(fd, sizeof(RingBufferControl) + 4096);
    void* addr = mmap(NULL, sizeof(RingBufferControl) + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    
    RingBufferControl* control = (RingBufferControl*)addr;
    control->ref_count = 1;
    control->writer_pid = 999999; // Unlikely to exist
    control->writer_heartbeat = time(NULL) - 100;
    
    munmap(addr, sizeof(RingBufferControl) + 4096);
    close(fd);
    
    // Should clean up
    size_t cleaned = LargeDataChannel::cleanupOrphanedChannels(10);
    ASSERT_EQ(cleaned, 1);
    
    // Verify file gone
    fd = shm_open(channel_name.c_str(), O_RDONLY, 0666);
    ASSERT_EQ(fd, -1);
}
