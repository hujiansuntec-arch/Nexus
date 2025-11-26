/**
 * @file test_cross_process_node_events.cpp
 * @brief Test cross-process NODE_JOINED and NODE_LEFT events
 * 
 * This test demonstrates:
 * 1. NODE_JOINED events across processes
 * 2. NODE_LEFT events when nodes disconnect
 * 3. Service cleanup when nodes leave
 */

#include "Node.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

using namespace librpc;

struct EventRecord {
    ServiceEvent event;
    std::string node_id;
    std::string capability;
    
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

void recordEvent(ServiceEvent event, const ServiceDescriptor& svc) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.push_back({event, svc.node_id, svc.getCapability()});
    std::cout << "[Event #" << events.size() << "] " << events.back().toString() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <observer|joiner>" << std::endl;
        return 1;
    }
    
    std::string role = argv[1];
    
    if (role == "observer") {
        std::cout << "\n=== Observer Process (watches for node events) ===" << std::endl;
        
        // Create observer node first
        auto observer = createNode("observer_node");
        observer->setServiceDiscoveryCallback(recordEvent);
        
        std::cout << "[Observer] Node created and ready" << std::endl;
        std::cout << "[Observer] Waiting for other nodes to join..." << std::endl;
        std::cout << "[Observer] (Start 'joiner' process now, then stop it to test NODE_LEFT)" << std::endl;
        std::cout << "[Observer] (Press Ctrl+C to exit)" << std::endl;
        
        // Keep running and displaying events
        int last_count = 0;
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            int current_count = events.size();
            if (current_count != last_count) {
                std::cout << "\n[Observer] Total events so far: " << current_count << std::endl;
                last_count = current_count;
            }
        }
        
    } else if (role == "joiner") {
        std::cout << "\n=== Joiner Process (joins and leaves) ===" << std::endl;
        
        // Wait a bit for observer to start
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        // Create joiner node
        auto joiner = createNode("joiner_node");
        joiner->setServiceDiscoveryCallback(recordEvent);
        
        std::cout << "[Joiner] Node created" << std::endl;
        
        // Wait to see if we receive NODE_JOINED from observer
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            std::cout << "\n[Joiner] Received " << events.size() << " events:" << std::endl;
            for (const auto& e : events) {
                std::cout << "  - " << e.toString() << std::endl;
            }
        }
        
        // Register a test service
        std::cout << "\n[Joiner] Registering test service..." << std::endl;
        joiner->subscribe("test", {"data"}, 
            [](const std::string&, const std::string&, const uint8_t*, size_t) {
                // Dummy callback
            });
        
        std::cout << "[Joiner] Service registered" << std::endl;
        std::cout << "[Joiner] Running for 3 seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        std::cout << "[Joiner] Exiting (this should trigger NODE_LEFT in observer)..." << std::endl;
        
        // Node will be destroyed here, triggering NODE_LEFT
        
    } else {
        std::cerr << "Invalid role: " << role << std::endl;
        std::cerr << "Must be 'observer' or 'joiner'" << std::endl;
        return 1;
    }
    
    return 0;
}
