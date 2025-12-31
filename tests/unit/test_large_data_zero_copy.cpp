#include "simple_test.h"
#include "nexus/transport/LargeDataChannel.h"
#include <vector>
#include <numeric>
#include <cstring>

using namespace Nexus::rpc;

// Helper for cleanup
void cleanup() {
    LargeDataChannel::cleanupOrphanedChannels(0);
}

TEST(LargeDataZeroCopyTest, AllocWrite_Alignment) {
    cleanup();
    LargeDataChannel::Config config;
    config.buffer_size = 1024 * 1024; // 1MB
    auto channel = LargeDataChannel::create("test_zero_copy_align", config);
    ASSERT_TRUE(channel != nullptr);

    // Allocate a small block
    auto block = channel->allocWrite(100);
    ASSERT_TRUE(block.isValid());
    ASSERT_NE(block.data, nullptr);

    // Check alignment of the data pointer
    // The header is 128 bytes (aligned to 64), so data should also be aligned to 64
    uintptr_t addr = reinterpret_cast<uintptr_t>(block.data);
    ASSERT_EQ(addr % 64, 0);

    // Commit
    channel->commitWrite(block, "topic1");
    cleanup();
}

TEST(LargeDataZeroCopyTest, ZeroCopy_WriteRead) {
    cleanup();
    LargeDataChannel::Config config;
    config.buffer_size = 1024 * 1024;
    auto channel = LargeDataChannel::create("test_zero_copy_wr", config);
    ASSERT_TRUE(channel != nullptr);

    // Create a reader
    auto reader = LargeDataChannel::create("test_zero_copy_wr", config);
    ASSERT_TRUE(reader != nullptr);

    // Prepare data
    size_t data_size = 1024;
    std::vector<uint8_t> expected_data(data_size);
    std::iota(expected_data.begin(), expected_data.end(), 0);

    // Zero-copy write
    auto block = channel->allocWrite(data_size);
    ASSERT_TRUE(block.isValid());
    std::memcpy(block.data, expected_data.data(), data_size);
    channel->commitWrite(block, "topic_zc");

    // Read back
    LargeDataChannel::DataBlock read_block;
    ASSERT_TRUE(reader->tryRead(read_block));
    ASSERT_EQ((int)read_block.result, (int)LargeDataChannel::ReadResult::SUCCESS);
    ASSERT_EQ(read_block.size, data_size);
    ASSERT_EQ(std::memcmp(read_block.data, expected_data.data(), data_size), 0);
    
    reader->releaseBlock(read_block);
    cleanup();
}

TEST(LargeDataZeroCopyTest, ConfigurableCRC32) {
    cleanup();
    LargeDataChannel::Config config;
    config.buffer_size = 1024 * 1024;
    config.enable_crc32 = false; // Disable CRC32
    auto channel = LargeDataChannel::create("test_crc_disable", config);
    ASSERT_TRUE(channel != nullptr);

    auto block = channel->allocWrite(100);
    ASSERT_TRUE(block.isValid());
    std::memset(block.data, 0xAA, 100);
    channel->commitWrite(block, "topic_no_crc");

    auto reader = LargeDataChannel::create("test_crc_disable", config);
    ASSERT_TRUE(reader != nullptr);
    
    LargeDataChannel::DataBlock read_block;
    ASSERT_TRUE(reader->tryRead(read_block));
    ASSERT_EQ((int)read_block.result, (int)LargeDataChannel::ReadResult::SUCCESS);
    
    // If CRC was disabled, the header->crc32 should be 0.
    ASSERT_EQ(read_block.header->crc32, 0);
    
    reader->releaseBlock(read_block);
    cleanup();
}

TEST(LargeDataZeroCopyTest, Alignment_Offset_Check) {
    cleanup();
    LargeDataChannel::Config config;
    config.buffer_size = 1024 * 1024;
    auto channel = LargeDataChannel::create("test_align_offset", config);
    ASSERT_TRUE(channel != nullptr);

    // Write 1 byte. 
    // Header (128) + Data (1) = 129.
    // Aligned to 64 bytes -> 192 bytes (128 + 64).
    // So next write should start at offset 192.
    
    auto block1 = channel->allocWrite(1);
    uintptr_t addr1 = reinterpret_cast<uintptr_t>(block1.data);
    channel->commitWrite(block1, "t1");

    auto block2 = channel->allocWrite(1);
    uintptr_t addr2 = reinterpret_cast<uintptr_t>(block2.data);
    channel->commitWrite(block2, "t2");

    size_t diff = addr2 - addr1;
    // Note: addr1 and addr2 are pointers to DATA.
    // block1.data is at offset X + 128
    // block2.data is at offset X + 192 + 128
    // So diff should be 192.
    ASSERT_EQ(diff, 192);
    cleanup();
}
