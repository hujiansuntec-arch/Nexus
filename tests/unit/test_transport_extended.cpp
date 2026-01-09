#include "simple_test.h"
#include "nexus/transport/SharedMemoryTransportV3.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>

using namespace Nexus;
using namespace Nexus::rpc;

TEST(SharedMemoryTransportExtendedTest, PointToPoint) {
    SharedMemoryTransportV3 t1;
    SharedMemoryTransportV3 t2;
    
    // Transport initialization with process-level ID
    ASSERT_TRUE(t1.initialize("proc_t1"));
    ASSERT_TRUE(t1.registerNodeToRegistry("node1"));
    
    ASSERT_TRUE(t2.initialize("proc_t2"));
    ASSERT_TRUE(t2.registerNodeToRegistry("node2"));
    
    std::atomic<int> received_count{0};
    std::string last_msg;
    std::string last_sender;
    
    // Register callback for node2
    t2.addReceiveCallback("node2", [&](const uint8_t* data, size_t size, const std::string& from) {
        last_msg.assign(reinterpret_cast<const char*>(data), size);
        last_sender = from;
        received_count++;
    });
    
    t2.startReceiving();
    
    std::string msg = "Hello Node2";
    ASSERT_TRUE(t1.send("node2", reinterpret_cast<const uint8_t*>(msg.c_str()), msg.size()));
    
    // Wait for delivery
    for (int i = 0; i < 20; ++i) {
        if (received_count > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    ASSERT_EQ(1, received_count);
    ASSERT_EQ(msg, last_msg);
    // In V3 Process-Level Transport, the sender is identified by the Transport ID (Process ID),
    // not the individual Node ID. The Node ID is contained within the message payload (packet header),
    // which this raw test doesn't use.
    ASSERT_EQ("proc_t1", last_sender);
    
    t2.stopReceiving();
}

TEST(SharedMemoryTransportExtendedTest, Broadcast) {
    SharedMemoryTransportV3 t1;
    SharedMemoryTransportV3 t2;
    SharedMemoryTransportV3 t3;
    
    ASSERT_TRUE(t1.initialize("proc_b1"));
    ASSERT_TRUE(t1.registerNodeToRegistry("b_node1"));
    
    ASSERT_TRUE(t2.initialize("proc_b2"));
    ASSERT_TRUE(t2.registerNodeToRegistry("b_node2"));
    
    ASSERT_TRUE(t3.initialize("proc_b3"));
    ASSERT_TRUE(t3.registerNodeToRegistry("b_node3"));
    
    std::atomic<int> count2{0};
    std::atomic<int> count3{0};
    
    t2.addReceiveCallback("b_node2", [&](const uint8_t*, size_t, const std::string&) { count2++; });
    t3.addReceiveCallback("b_node3", [&](const uint8_t*, size_t, const std::string&) { count3++; });
    
    t2.startReceiving();
    t3.startReceiving();
    
    // Wait for registry update (broadcast relies on knowing other nodes)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::string msg = "Broadcast Msg";
    int sent_count = t1.broadcast(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.size());
    
    // Should send to at least 2 nodes (t2, t3)
    // Note: broadcast might send to self if not filtered, but usually filters self.
    // Also depends on registry discovery.
    
    // Wait for delivery
    for (int i = 0; i < 20; ++i) {
        if (count2 > 0 && count3 > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    ASSERT_EQ(1, count2);
    ASSERT_EQ(1, count3);
    
    t2.stopReceiving();
    t3.stopReceiving();
}
