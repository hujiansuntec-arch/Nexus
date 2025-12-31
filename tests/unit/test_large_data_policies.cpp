#include "simple_test.h"
#include "nexus/transport/LargeDataChannel.h"
#include <vector>
#include <string>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

using namespace Nexus::rpc;

TEST(LargeDataPolicies, DropOldest) {
    LargeDataChannel::Config config;
    config.buffer_size = 4096 * 10;
    config.max_block_size = 4096;
    config.overflow_policy = LargeDataOverflowPolicy::DROP_OLDEST;
    
    std::string shm_name = "test_policy_drop_oldest";
    shm_unlink(shm_name.c_str());
    
    auto channel = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(channel != nullptr);

    // Create a reader to hold the read position
    auto reader = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(reader != nullptr);
    LargeDataChannel::DataBlock block;
    reader->tryRead(block); // Registers the reader
    
    std::vector<uint8_t> data(1024, 0xAA);
    
    // Fill buffer and overflow
    // Capacity is 40960. Each write is ~1100 bytes.
    // 50 writes should definitely overflow.
    int writes = 0;
    for (int i = 0; i < 50; ++i) {
        int64_t seq = channel->write("topic", data.data(), data.size());
        ASSERT_NE(seq, -1); // Should always succeed with DROP_OLDEST
        writes++;
    }
    
    // Reader should have missed data
    // Try to read. The first block should have a sequence number > 0
    bool has_data = reader->tryRead(block);
    ASSERT_TRUE(has_data);
    ASSERT_TRUE(block.isValid());
    
    // Since we wrote 50 blocks, and buffer holds ~30, we must have dropped ~20.
    // So the first available block should be around sequence 20.
    std::cout << "First read sequence: " << block.header->sequence << std::endl;
    ASSERT_GT(block.header->sequence, 0);
    
    reader->releaseBlock(block);
}

TEST(LargeDataPolicies, Block) {
    LargeDataChannel::Config config;
    config.buffer_size = 4096 * 10;
    config.max_block_size = 4096;
    config.overflow_policy = LargeDataOverflowPolicy::BLOCK;
    
    std::string shm_name = "test_policy_block";
    shm_unlink(shm_name.c_str());
    
    auto channel = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(channel != nullptr);

    // Create a reader to hold the read position
    auto reader = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(reader != nullptr);
    LargeDataChannel::DataBlock block;
    reader->tryRead(block); // Registers the reader
    
    std::vector<uint8_t> data(1024, 0xAA);
    
    // Fill buffer
    int writes = 0;
    for (int i = 0; i < 50; ++i) {
        if (channel->write("topic", data.data(), data.size()) == -1) {
            break;
        }
        writes++;
    }
    
    // Should have stopped before 50
    ASSERT_LT(writes, 50);
    
    // Should log error and return -1 (BLOCK not recommended/implemented fully?)
    // The code says: NEXUS_ERROR("LargeData") << "Buffer full (BLOCK policy not recommended)";
    int64_t result = channel->write("topic", data.data(), data.size());
    ASSERT_EQ(result, -1);
}

TEST(LargeDataPolicies, WriteTooLarge) {
    LargeDataChannel::Config config;
    config.max_block_size = 1024;
    
    std::string shm_name = "test_policy_too_large";
    shm_unlink(shm_name.c_str());
    
    auto channel = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(channel != nullptr);
    
    std::vector<uint8_t> data(2048, 0xAA);
    
    // Should fail
    int64_t result = channel->write("topic", data.data(), data.size());
    ASSERT_EQ(result, -1);
}

TEST(LargeDataPolicies, DropNewest) {
    LargeDataChannel::Config config;
    config.buffer_size = 4096 * 10;
    config.max_block_size = 4096;
    config.overflow_policy = LargeDataOverflowPolicy::DROP_NEWEST;
    
    bool callback_called = false;
    config.overflow_callback = [&](const std::string&, const std::string&, uint64_t, size_t) {
        callback_called = true;
    };
    
    std::string shm_name = "test_policy_drop_newest";
    shm_unlink(shm_name.c_str());
    
    auto channel = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(channel != nullptr);

    // Create a reader to hold the read position
    auto reader = LargeDataChannel::create(shm_name, config);
    ASSERT_TRUE(reader != nullptr);
    LargeDataChannel::DataBlock block;
    reader->tryRead(block); // Registers the reader
    
    std::vector<uint8_t> data(1024, 0xAA);
    
    // Fill buffer
    int writes = 0;
    for (int i = 0; i < 50; ++i) {
        if (channel->write("topic", data.data(), data.size()) == -1) {
            break;
        }
        writes++;
    }
    
    // Should have stopped before 50
    ASSERT_LT(writes, 50);
    
    // The next write should fail and call callback
    int64_t result = channel->write("topic", data.data(), data.size());
    ASSERT_EQ(result, -1);
    ASSERT_TRUE(callback_called);
}
