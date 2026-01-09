#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>

using namespace Nexus::rpc;

TEST(TransportStress, NotifyMechanisms) {
    // Test SEMAPHORE
    {
        SharedMemoryTransportV3 t1, t2;
        SharedMemoryTransportV3::Config config;
        config.notify_mechanism = SharedMemoryTransportV3::NotifyMechanism::SEMAPHORE;
        
        ASSERT_TRUE(t1.initialize("proc_sem1", config));
        ASSERT_TRUE(t1.registerNodeToRegistry("sem_node1"));

        ASSERT_TRUE(t2.initialize("proc_sem2", config));
        ASSERT_TRUE(t2.registerNodeToRegistry("sem_node2"));
        
        std::atomic<int> received{0};
        t2.addReceiveCallback("sem_node2", [&](const uint8_t*, size_t, const std::string&) { received++; });
        t2.startReceiving();
        
        std::string msg = "sem_msg";
        // Wait a bit for discovery/connection
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ASSERT_TRUE(t1.send("sem_node2", (const uint8_t*)msg.data(), msg.size()));
        
        for(int i=0; i<50 && received==0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ASSERT_EQ(received, 1);
        t2.stopReceiving();
    }
    
    // Test SMART_POLLING
    {
        SharedMemoryTransportV3 t1, t2;
        SharedMemoryTransportV3::Config config;
        config.notify_mechanism = SharedMemoryTransportV3::NotifyMechanism::SMART_POLLING;
        
        ASSERT_TRUE(t1.initialize("proc_poll1", config));
        ASSERT_TRUE(t1.registerNodeToRegistry("poll_node1"));

        ASSERT_TRUE(t2.initialize("proc_poll2", config));
        ASSERT_TRUE(t2.registerNodeToRegistry("poll_node2"));
        
        std::atomic<int> received{0};
        t2.addReceiveCallback("poll_node2", [&](const uint8_t*, size_t, const std::string&) { received++; });
        t2.startReceiving();
        
        std::string msg = "poll_msg";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ASSERT_TRUE(t1.send("poll_node2", (const uint8_t*)msg.data(), msg.size()));
        
        for(int i=0; i<50 && received==0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ASSERT_EQ(received, 1);
        t2.stopReceiving();
    }
}

TEST(TransportStress, Congestion) {
    SharedMemoryTransportV3 t1, t2;
    SharedMemoryTransportV3::Config config;
    // Note: queue_capacity in Config is currently ignored by implementation, 
    // it uses fixed compile-time constant QUEUE_CAPACITY = 256.
    // So we need to send more than 256 messages to trigger congestion.
    
    ASSERT_TRUE(t1.initialize("proc_cong1", config));
    ASSERT_TRUE(t1.registerNodeToRegistry("cong_node1"));

    ASSERT_TRUE(t2.initialize("proc_cong2", config));
    ASSERT_TRUE(t2.registerNodeToRegistry("cong_node2"));
    
    // Don't start receiving yet to fill queue
    
    // Send many messages (more than capacity)
    // Queue capacity is 512KB.
    // If we send 2000 bytes per message, capacity is ~261 messages.
    // We send 300 messages to trigger overflow.
    int sent = 0;
    int total_to_send = 300;
    std::vector<uint8_t> data(2000, 0);
    
    // Wait for connection establishment
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    for(int i=0; i<total_to_send; ++i) {
        data[0] = static_cast<uint8_t>(i % 256); // Use byte for ID
        // We also put the full index in the payload to verify
        if (data.size() >= 4) {
            *reinterpret_cast<int*>(data.data()) = i;
        }
        
        if(t1.send("cong_node2", data.data(), data.size())) {
            sent++;
        }
    }
    
    // With Ring Buffer (SPSC), producer cannot move tail, so it cannot overwrite oldest.
    // Instead, it returns false (Drop Newest / Block).
    // So sent count should be less than total (we hit capacity).
    ASSERT_LT(sent, total_to_send);
    
    // Now start receiving to clear
    std::atomic<int> received{0};
    std::vector<int> received_ids;
    std::mutex received_mutex;

    t2.addReceiveCallback("cong_node2", [&](const uint8_t* d, size_t, const std::string&) { 
        received++; 
        std::lock_guard<std::mutex> lock(received_mutex);
        if (d) {
            received_ids.push_back(*reinterpret_cast<const int*>(d));
        }
    });
    t2.startReceiving();
    
    for(int i=0; i<100; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Should receive exactly what was sent
    ASSERT_EQ(received, sent);
    ASSERT_GT(received, 0);
    
    // Verify we got the FIRST messages (Drop Newest behavior)
    if (received > 0) {
        std::lock_guard<std::mutex> lock(received_mutex);
        // The last message sent (299) should be dropped because queue was full
        bool found_last = false;
        for(int id : received_ids) {
            if(id == 299) found_last = true;
        }
        ASSERT_FALSE(found_last);
        
        // The first message (0) should be present
        bool found_first = false;
        for(int id : received_ids) {
            if(id == 0) found_first = true;
        }
        ASSERT_TRUE(found_first);
    }
    
    t2.stopReceiving();
}

TEST(TransportStress, QueueLimit) {
    SharedMemoryTransportV3 receiver;
    SharedMemoryTransportV3::Config config;
    config.max_inbound_queues = 5; // Set low limit
    
    ASSERT_TRUE(receiver.initialize("proc_limit_recv", config));
    ASSERT_TRUE(receiver.registerNodeToRegistry("limit_recv"));
    
    std::vector<std::unique_ptr<SharedMemoryTransportV3>> senders;
    int success_count = 0;
    
    // Try to connect 10 senders
    for(int i=0; i<10; ++i) {
        auto sender = std::make_unique<SharedMemoryTransportV3>();
        std::string node_id = "limit_sender_" + std::to_string(i);
        std::string proc_id = "proc_" + node_id;

        if(sender->initialize(proc_id)) {
            sender->registerNodeToRegistry(node_id);

            // Try to send to receiver (triggers queue creation)
            std::string msg = "hello";
            // Wait a bit for registry propagation
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if(sender->send("limit_recv", (const uint8_t*)msg.data(), msg.size())) {
                success_count++;
            }
        }
        senders.push_back(std::move(sender));
    }
    
    // Should be limited by max_inbound_queues (5)
    // Note: The implementation checks num_queues >= max_queues.
    // If 5 queues are created, the 6th attempt should fail.
    ASSERT_TRUE(success_count <= 5);
}

TEST(TransportStress, DynamicQueueAddition) {
    SharedMemoryTransportV3 receiver;
    ASSERT_TRUE(receiver.initialize("proc_dyn_recv"));
    ASSERT_TRUE(receiver.registerNodeToRegistry("dyn_recv"));
    
    std::atomic<int> received{0};
    receiver.addReceiveCallback("dyn_recv", [&](const uint8_t*, size_t, const std::string&) { received++; });
    receiver.startReceiving();
    
    // Add sender 1
    SharedMemoryTransportV3 s1;
    ASSERT_TRUE(s1.initialize("proc_dyn_s1"));
    ASSERT_TRUE(s1.registerNodeToRegistry("dyn_s1"));

    std::string msg = "msg1";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(s1.send("dyn_recv", (const uint8_t*)msg.data(), msg.size()));
    
    // Wait for receive
    for(int i=0; i<50 && received < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(received, 1);
    
    // Add sender 2 while receiving
    SharedMemoryTransportV3 s2;
    ASSERT_TRUE(s2.initialize("proc_dyn_s2"));
    ASSERT_TRUE(s2.registerNodeToRegistry("dyn_s2"));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(s2.send("dyn_recv", (const uint8_t*)msg.data(), msg.size()));
    
    // Wait for receive
    for(int i=0; i<50 && received < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(received, 2);
    
    receiver.stopReceiving();
}

TEST(TransportStress, ExplicitDisconnect) {
    SharedMemoryTransportV3 t1, t2;
    ASSERT_TRUE(t1.initialize("disc_node1"));
    ASSERT_TRUE(t2.initialize("disc_node2"));
    
    // Register nodes to ensure they exist in registry
    ASSERT_TRUE(t1.registerNodeToRegistry("disc_node1"));
    ASSERT_TRUE(t2.registerNodeToRegistry("disc_node2"));
    
    std::string msg = "hello";
    // Connect
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(t1.send("disc_node2", (const uint8_t*)msg.data(), msg.size()));
    
    // Verify connection exists
    ASSERT_EQ(t1.getConnectionCount(), 1);
    
    // Disconnect
    t1.disconnectFromNode("disc_node2");
    ASSERT_EQ(t1.getConnectionCount(), 0);
    
    // Send again - should reconnect
    ASSERT_TRUE(t1.send("disc_node2", (const uint8_t*)msg.data(), msg.size()));
    ASSERT_EQ(t1.getConnectionCount(), 1);
}

TEST(TransportStress, HeartbeatLoop) {
    SharedMemoryTransportV3 t;
    ASSERT_TRUE(t.initialize("hb_node"));
    t.startReceiving();
    
    // Wait for heartbeat update (interval is 1s)
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    
    t.stopReceiving();
}

TEST(TransportStress, Stats) {
    SharedMemoryTransportV3 t;
    SharedMemoryTransportV3::Config config;
    config.enable_stats = true;
    ASSERT_TRUE(t.initialize("stats_node", config));
    ASSERT_TRUE(t.registerNodeToRegistry("stats_node"));
    
    auto stats = t.getStats();
    ASSERT_EQ(stats.messages_sent, 0);
    ASSERT_EQ(stats.messages_received, 0);
    
    // Send to self (should fail as self-send is usually blocked or handled differently)
    // But we can send to another node
    SharedMemoryTransportV3 t2;
    ASSERT_TRUE(t2.initialize("stats_node2", config));
    ASSERT_TRUE(t2.registerNodeToRegistry("stats_node2"));
    t2.startReceiving();
    
    std::string msg = "stats";
    t.send("stats_node2", (const uint8_t*)msg.data(), msg.size());
    
    // Wait for stats update
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    stats = t.getStats();
    ASSERT_EQ(stats.messages_sent, 1);
    
    auto stats2 = t2.getStats();
    // Receive stats might need time or synchronization
    for(int i=0; i<50; ++i) {
        stats2 = t2.getStats();
        if(stats2.messages_received == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(stats2.messages_received, 1);
    
    t2.stopReceiving();
}
