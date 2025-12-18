#include "simple_test.h"
#include "nexus/core/Config.h"
#include <cstdlib>

using namespace Nexus::rpc;

TEST(ConfigTest, DefaultValues) {
    Config& config = Config::instance();
    
    ASSERT_GT(config.node.num_processing_threads, 0);
    ASSERT_GT(config.node.max_queue_size, 0);
    ASSERT_GT(config.shm.queue_capacity, 0);
}

TEST(ConfigTest, LoadFromEnv) {
    // Set environment variables
    setenv("NEXUS_NUM_THREADS", "16", 1);
    setenv("NEXUS_MAX_QUEUE_SIZE", "5000", 1);
    
    Config& config = Config::instance();
    config.loadFromEnv();
    
    ASSERT_EQ(16, config.node.num_processing_threads);
    ASSERT_EQ(5000, config.node.max_queue_size);
    
    // Reset env
    unsetenv("NEXUS_NUM_THREADS");
    unsetenv("NEXUS_MAX_QUEUE_SIZE");
}
