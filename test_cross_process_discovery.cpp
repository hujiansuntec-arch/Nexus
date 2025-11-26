/**
 * @file test_cross_process_discovery.cpp
 * @brief Test cross-process service discovery functionality
 * 
 * This test demonstrates:
 * 1. Service discovery across processes (via shared memory)
 * 2. NODE_JOINED / NODE_LEFT events
 * 3. SERVICE_ADDED / SERVICE_REMOVED events from other processes
 * 4. Automatic service synchronization
 */

#include "Node.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>

using namespace librpc;

std::atomic<int> event_count{0};

void printServiceEvent(ServiceEvent event, const ServiceDescriptor& svc) {
    event_count++;
    
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
    
    std::cout << "[Event #" << event_count << "] " << event_name 
              << " - Node: " << svc.node_id 
              << ", Capability: " << svc.getCapability() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <provider|consumer>" << std::endl;
        return 1;
    }
    
    std::string role = argv[1];
    
    if (role == "provider") {
        std::cout << "\n=== Service Provider Process ===" << std::endl;
        
        // Create provider node
        auto provider = createNode("provider_node");
        
        // Set callback to monitor events
        provider->setServiceDiscoveryCallback(printServiceEvent);
        
        std::cout << "[Provider] Node created, registering services..." << std::endl;
        
        // Register temperature sensor service
        provider->subscribe("sensor", {"temperature"}, 
            [](const std::string&, const std::string&, const uint8_t*, size_t) {
                // Dummy callback
            });
        
        std::cout << "[Provider] Registered temperature service" << std::endl;
        
        // Register large data service (camera)
        uint8_t dummy_data[1024] = {0};
        provider->sendLargeData("sensor", "camera", "frame", dummy_data, sizeof(dummy_data));
        
        std::cout << "[Provider] Registered camera service" << std::endl;
        std::cout << "[Provider] Waiting for consumer process..." << std::endl;
        std::cout << "[Provider] (Press Ctrl+C to exit)" << std::endl;
        
        // Keep running
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } else if (role == "consumer") {
        std::cout << "\n=== Service Consumer Process ===" << std::endl;
        
        // Wait a bit for provider to start
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Create consumer node
        auto consumer = createNode("consumer_node");
        
        // Set callback to monitor all events
        consumer->setServiceDiscoveryCallback(printServiceEvent);
        
        std::cout << "[Consumer] Node created, discovering services..." << std::endl;
        
        // Wait for service discovery messages to propagate
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Discover all services
        std::cout << "\n--- Discovering All Services ---" << std::endl;
        auto all_services = consumer->discoverServices();
        std::cout << "[Consumer] Found " << all_services.size() << " services:" << std::endl;
        for (const auto& svc : all_services) {
            std::cout << "  - Node: " << svc.node_id 
                      << ", Type: " << (svc.type == ServiceType::NORMAL_MESSAGE ? "NORMAL" : "LARGE_DATA")
                      << ", Capability: " << svc.getCapability() << std::endl;
        }
        
        // Find specific service
        std::cout << "\n--- Finding Temperature Service ---" << std::endl;
        auto temp_nodes = consumer->findNodesByCapability("sensor/temperature");
        std::cout << "[Consumer] Nodes providing temperature: " << temp_nodes.size() << std::endl;
        for (const auto& node_id : temp_nodes) {
            std::cout << "  - " << node_id << std::endl;
        }
        
        // Find large data channels
        std::cout << "\n--- Finding Large Data Channels ---" << std::endl;
        auto channels = consumer->findLargeDataChannels("sensor");
        std::cout << "[Consumer] Found " << channels.size() << " large data channel(s):" << std::endl;
        for (const auto& ch : channels) {
            std::cout << "  - Channel: " << ch.channel_name 
                      << ", Topic: " << ch.topic 
                      << ", Node: " << ch.node_id << std::endl;
        }
        
        std::cout << "\n=== Test Summary ===" << std::endl;
        std::cout << "Total events received: " << event_count.load() << std::endl;
        std::cout << "Services discovered: " << all_services.size() << std::endl;
        std::cout << "Temperature providers: " << temp_nodes.size() << std::endl;
        std::cout << "Large data channels: " << channels.size() << std::endl;
        
        if (all_services.size() >= 2 && temp_nodes.size() >= 1 && channels.size() >= 1) {
            std::cout << "\n✓ Cross-process service discovery test PASSED!" << std::endl;
            return 0;
        } else {
            std::cout << "\n✗ Cross-process service discovery test FAILED!" << std::endl;
            return 1;
        }
        
    } else {
        std::cerr << "Invalid role: " << role << std::endl;
        std::cerr << "Must be 'provider' or 'consumer'" << std::endl;
        return 1;
    }
    
    return 0;
}
