#include "simple_test.h"
#include "nexus/utils/Logger.h"

using namespace Nexus;
using namespace Nexus::rpc;

TEST(LoggerTest, SetLevel) {
    auto& logger = Logger::instance();
    logger.setLevel(Logger::Level::DEBUG);
    
    // Cast to int for comparison because Logger::Level doesn't have operator<<
    ASSERT_EQ(static_cast<int>(Logger::Level::DEBUG), static_cast<int>(logger.getLevel()));
    
    logger.setLevel(Logger::Level::ERROR);
    ASSERT_EQ(static_cast<int>(Logger::Level::ERROR), static_cast<int>(logger.getLevel()));
}

TEST(LoggerTest, Logging) {
    // Just ensure it doesn't crash
    NEXUS_LOG_INFO("Test", "Test info message");
    NEXUS_LOG_ERROR("Test", "Test error message");
}
