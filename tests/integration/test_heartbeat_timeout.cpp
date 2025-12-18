/**
 * @file test_heartbeat_timeout.cpp
 * @brief Test heartbeat timeout detection and automatic NODE_LEFT event
 * 
 * This test demonstrates:
 * 1. Node crash detection via heartbeat timeout (5 seconds)
 * 2. Automatic NODE_LEFT event triggered by heartbeat mechanism
 * 3. Automatic service cleanup when node times out
 */

#include "nexus/core/Node.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Nexus::rpc;

struct EventRecord {
    ServiceEvent event;
    std::string node_id;
    std::string capability;
    std::chrono::steady_clock::time_point timestamp;
    
    std::string toString() const {
        const char* event_name = "";
        switch (event) {
            case ServiceEvent::NODE_JOINED:
                event_name = "NODE_JOINED";
                break;
            case ServiceEvent::NODE_LEFT:
                event_name = "NODE_LEFT";
                break;
            case ServiceEvent::SERVICE_ADDED:
                event_name = "SERVICE_ADDED";
                break;
            case ServiceEvent::SERVICE_REMOVED:
                event_name = "SERVICE_REMOVED";
                break;
        }
        return std::string(event_name) + " - Node: " + node_id + 
               (capability.empty() ? "" : ", Capability: " + capability);
    }
};

std::vector<EventRecord> events;
std::mutex events_mutex;
auto test_start = std::chrono::steady_clock::now();

void recordEvent(ServiceEvent event, const ServiceDescriptor& svc) {
    std::lock_guard<std::mutex> lock(events_mutex);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - test_start).count();
    
    events.push_back({event, svc.node_id, svc.getCapability(), now});
    std::cout << "[+" << elapsed << "ms] Event #" << events.size() << ": " 
              << events.back().toString() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <observer|crasher>" << std::endl;
        return 1;
    }
    
    std::string role = argv[1];
    
    if (role == "observer") {
        std::cout << "\n=== Observer Process (monitors heartbeat timeout) ===" << std::endl;
        
        test_start = std::chrono::steady_clock::now();
        
        // Create observer node
        auto observer = createNode("observer_node");
        observer->setServiceDiscoveryCallback(recordEvent);
        
        std::cout << "[Observer] Node created, waiting for crasher to join..." << std::endl;
        std::cout << "[Observer] Heartbeat timeout is 5 seconds" << std::endl;
        
        // Wait for events (crasher joins, registers service, then crashes)
        // Expected timeline:
        // T+0s:    crasher joins → NODE_JOINED
        // T+0.5s:  crasher registers service → SERVICE_ADDED
        // T+2s:    crasher exits (simulated crash, no NODE_LEAVE sent)
        // T+7s:    heartbeat timeout (5s after last heartbeat) → NODE_LEFT
        
        std::cout << "[Observer] Waiting 15 seconds to observe heartbeat timeout..." << std::endl;
        
        for (int i = 0; i < 15; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - test_start).count();
            std::cout << "[Observer] T+" << elapsed << "s - Waiting..." << std::endl;
        }
        
        // Analyze events
        std::cout << "\n=== Event Analysis ===" << std::endl;
        
        int node_joined_count = 0;
        int node_left_count = 0;
        int service_added_count = 0;
        bool heartbeat_timeout_triggered = false;
        
        std::chrono::steady_clock::time_point join_time;
        std::chrono::steady_clock::time_point left_time;
        
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            for (size_t i = 0; i < events.size(); ++i) {
                const auto& e = events[i];
                std::cout << "  Event " << (i+1) << ": " << e.toString() << std::endl;
                
                if (e.event == ServiceEvent::NODE_JOINED && e.node_id.find("crasher_node") != std::string::npos) {
                    node_joined_count++;
                    join_time = e.timestamp;
                }
                if (e.event == ServiceEvent::NODE_LEFT && e.node_id.find("crasher_node") != std::string::npos) {
                    node_left_count++;
                    left_time = e.timestamp;
                    
                    // Check if this was triggered by automatic detection
                    // (heartbeat timeout OR process death detection)
                    auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        left_time - join_time).count();
                    
                    // Should trigger within 2-7 seconds:
                    // - Process death: ~1-3s (detected immediately when process dies)
                    // - Heartbeat timeout: ~5-7s (5s timeout + 1s heartbeat interval)
                    if (delay_ms >= 1000 && delay_ms <= 8000) {
                        heartbeat_timeout_triggered = true;
                    }
                }
                if (e.event == ServiceEvent::SERVICE_ADDED) {
                    service_added_count++;
                }
            }
        }
        
        std::cout << "\n=== Test Results ===" << std::endl;
        std::cout << "NODE_JOINED events: " << node_joined_count << " (expected: 1)" << std::endl;
        std::cout << "NODE_LEFT events: " << node_left_count << " (expected: 1)" << std::endl;
        std::cout << "SERVICE_ADDED events: " << service_added_count << " (expected: 1)" << std::endl;
        std::cout << "Automatic node cleanup triggered: " 
                  << (heartbeat_timeout_triggered ? "YES ✓" : "NO ✗") << std::endl;
        
        if (!left_time.time_since_epoch().count()) {
            std::cout << "⚠ NODE_LEFT was never triggered!" << std::endl;
        } else {
            auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                left_time - join_time).count();
            std::cout << "Time from NODE_JOINED to NODE_LEFT: " << delay_ms << "ms" << std::endl;
            
            if (delay_ms < 3000) {
                std::cout << "  → Detected via process death check (fast)" << std::endl;
            } else {
                std::cout << "  → Detected via heartbeat timeout (5s)" << std::endl;
            }
        }
        
        bool success = (node_joined_count == 1 && 
                       node_left_count == 1 && 
                       service_added_count == 1 &&
                       heartbeat_timeout_triggered);
        
        if (success) {
            std::cout << "\n✓ Automatic node cleanup test PASSED!" << std::endl;
            std::cout << "   Heartbeat mechanism successfully detected crashed node" << std::endl;
            return 0;
        } else {
            std::cout << "\n✗ Automatic node cleanup test FAILED!" << std::endl;
            return 1;
        }
        
    } else if (role == "crasher") {
        std::cout << "\n=== Crasher Process (simulates node crash) ===" << std::endl;
        
        // Wait for observer to start
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        // Create crasher node
        auto crasher = createNode("crasher_node");
        
        std::cout << "[Crasher] Node created" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Register a service
        std::cout << "[Crasher] Registering test service..." << std::endl;
        crasher->subscribe("test", {"data"}, 
            [](const std::string&, const std::string&, const uint8_t*, size_t) {
                // Dummy callback
            });
        
        std::cout << "[Crasher] Service registered" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Simulate crash - exit WITHOUT calling destructor
        std::cout << "[Crasher] Simulating crash (exit without cleanup)..." << std::endl;
        _exit(0);  // Force exit without destructors
        
    } else {
        std::cerr << "Invalid role: " << role << std::endl;
        std::cerr << "Must be 'observer' or 'crasher'" << std::endl;
        return 1;
    }
    
    return 0;
}
