#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include "nexus/registry/GlobalRegistry.h"
#include <vector>
#include <string>
#include <thread>
#include <atomic>

using namespace Nexus::rpc;

// Helper for cleanup
struct TransportV3ModeCleanup {
    TransportV3ModeCleanup() { clear(); }
    ~TransportV3ModeCleanup() { clear(); }
    
    void clear() {
        GlobalRegistry::instance().clearServices();
    }
};

TEST(TransportV3Modes, SemaphoreMode) {
    TransportV3ModeCleanup cleanup;
    
    // 1. Create Receiver
    SharedMemoryTransportV3 receiver;
    SharedMemoryTransportV3::Config config;
    config.notify_mechanism = SharedMemoryTransportV3::NotifyMechanism::SEMAPHORE;
    ASSERT_TRUE(receiver.initialize("proc_sem_recv", config));
    ASSERT_TRUE(receiver.registerNodeToRegistry("sem_recv"));
    
    // Start receiving
    receiver.startReceiving();
    
    // 2. Create Sender
    SharedMemoryTransportV3 sender;
    // Sender MUST use the same mechanism because the mechanism is decided by the sender's config
    // when creating the queue in the receiver's shared memory.
    ASSERT_TRUE(sender.initialize("proc_sem_send", config));
    
    // 3. Send data
    std::vector<uint8_t> data = {1, 2, 3};
    ASSERT_TRUE(sender.send("sem_recv", data.data(), data.size()));
    
    // 4. Wait for delivery
    // We can check receiver stats
    for (int i = 0; i < 20; ++i) {
        auto stats = receiver.getStats();
        if (stats.messages_received > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    auto stats = receiver.getStats();
    ASSERT_EQ(stats.messages_received, 1);
    
    auto sender_stats = sender.getStats();
    ASSERT_EQ(sender_stats.messages_sent, 1);
}

TEST(TransportV3Modes, SmartPollingMode) {
    TransportV3ModeCleanup cleanup;
    
    // 1. Create Receiver
    SharedMemoryTransportV3 receiver;
    SharedMemoryTransportV3::Config config;
    config.notify_mechanism = SharedMemoryTransportV3::NotifyMechanism::SMART_POLLING;
    ASSERT_TRUE(receiver.initialize("proc_poll_recv", config));
    ASSERT_TRUE(receiver.registerNodeToRegistry("poll_recv"));
    
    receiver.startReceiving();
    
    // 2. Create Sender
    SharedMemoryTransportV3 sender;
    ASSERT_TRUE(sender.initialize("proc_poll_send", config));
    
    // 3. Send data
    std::vector<uint8_t> data = {4, 5, 6};
    ASSERT_TRUE(sender.send("poll_recv", data.data(), data.size()));
    
    // 4. Wait for delivery
    for (int i = 0; i < 20; ++i) {
        auto stats = receiver.getStats();
        if (stats.messages_received > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    auto stats = receiver.getStats();
    ASSERT_EQ(stats.messages_received, 1);
}

TEST(TransportV3Modes, ModeSwitching) {
    TransportV3ModeCleanup cleanup;
    
    // 1. Start with CV
    {
        SharedMemoryTransportV3 transport;
        SharedMemoryTransportV3::Config config;
        config.notify_mechanism = SharedMemoryTransportV3::NotifyMechanism::CONDITION_VARIABLE;
        ASSERT_TRUE(transport.initialize("switch_node", config));
        // Destructor will clean up
    }
    
    // 2. Restart with Semaphore (same node ID)
    {
        SharedMemoryTransportV3 transport;
        SharedMemoryTransportV3::Config config;
        config.notify_mechanism = SharedMemoryTransportV3::NotifyMechanism::SEMAPHORE;
        ASSERT_TRUE(transport.initialize("switch_node", config));
    }
}
