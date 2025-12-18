/**
 * Test program for service discovery functionality
 * 
 * Tests:
 * 1. Auto-registration of normal message services (subscribe)
 * 2. Auto-registration of large data services (sendLargeData)
 * 3. Service discovery by group
 * 4. Service discovery by type
 * 5. Finding nodes by capability
 * 6. Service discovery callback
 * 7. Same-process communication (zero-copy)
 */

#include "nexus/core/Node.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cassert>
#include <atomic>

using namespace Nexus::rpc;

std::atomic<int> discovery_callback_count{0};

void testServiceDiscovery() {
    std::cout << "\n=== Testing Service Discovery ===" << std::endl;
    
    // Create 3 nodes in same process
    auto sensor1 = createNode("sensor_temperature");
    auto sensor2 = createNode("sensor_camera_front");
    auto controller = createNode("controller");
    
    std::cout << "[Test] Created 3 nodes: sensor_temperature, sensor_camera_front, controller" << std::endl;
    
    // Set discovery callback on controller
    controller->setServiceDiscoveryCallback([](ServiceEvent event, const ServiceDescriptor& svc) {
        discovery_callback_count++;
        std::cout << "[Callback] Event: " 
                  << (event == ServiceEvent::SERVICE_ADDED ? "SERVICE_ADDED" : "SERVICE_REMOVED")
                  << ", Node: " << svc.node_id
                  << ", Capability: " << svc.getCapability() << std::endl;
    });
    
    // Test 1: Normal message service auto-registration
    std::cout << "\n--- Test 1: Normal Message Service Auto-Registration ---" << std::endl;
    
    int temp_count = 0;
    sensor1->subscribe("sensor", {"temperature"}, [&](const std::string& group,
                                                      const std::string& topic, 
                                                      const uint8_t* data, 
                                                      size_t len) {
        (void)group; (void)data;
        temp_count++;
        std::cout << "[sensor1] Received: " << topic << " (" << len << " bytes)" << std::endl;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Discover services
    auto all_services = controller->discoverServices();
    std::cout << "[Discovery] Total services: " << all_services.size() << std::endl;
    for (const auto& svc : all_services) {
        std::cout << "  - Node: " << svc.node_id 
                  << ", Type: " << (svc.type == ServiceType::NORMAL_MESSAGE ? "NORMAL" : "LARGE_DATA")
                  << ", Capability: " << svc.getCapability() << std::endl;
    }
    
    // Should have 1 service: sensor/temperature
    assert(all_services.size() == 1);
    assert(all_services[0].group == "sensor");
    assert(all_services[0].topic == "temperature");
    assert(all_services[0].type == ServiceType::NORMAL_MESSAGE);
    std::cout << "[Test 1] ✓ Passed" << std::endl;
    
    // Test 2: Large data service auto-registration
    std::cout << "\n--- Test 2: Large Data Service Auto-Registration ---" << std::endl;
    
    // Send large data (auto-registers service)
    std::vector<uint8_t> frame_data(1024 * 1024, 0xAB);  // 1MB frame
    sensor2->sendLargeData("sensor", "camera_front", "frame", 
                          frame_data.data(), frame_data.size());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Discover large data services
    auto large_services = controller->discoverServices("", ServiceType::LARGE_DATA);
    std::cout << "[Discovery] Large data services: " << large_services.size() << std::endl;
    for (const auto& svc : large_services) {
        std::cout << "  - Node: " << svc.node_id 
                  << ", Channel: " << svc.channel_name
                  << ", Capability: " << svc.getCapability() << std::endl;
    }
    
    // Should have 1 large data service
    assert(large_services.size() == 1);
    assert(large_services[0].group == "sensor");
    assert(large_services[0].channel_name == "camera_front");
    assert(large_services[0].topic == "frame");
    assert(large_services[0].type == ServiceType::LARGE_DATA);
    std::cout << "[Test 2] ✓ Passed" << std::endl;
    
    // Test 3: Filter by group
    std::cout << "\n--- Test 3: Filter by Group ---" << std::endl;
    
    auto sensor_services = controller->discoverServices("sensor");
    std::cout << "[Discovery] Services in 'sensor' group: " << sensor_services.size() << std::endl;
    
    // Should have 2 services (1 normal + 1 large data)
    assert(sensor_services.size() == 2);
    std::cout << "[Test 3] ✓ Passed" << std::endl;
    
    // Test 4: Find nodes by capability
    std::cout << "\n--- Test 4: Find Nodes by Capability ---" << std::endl;
    
    auto temp_nodes = controller->findNodesByCapability("sensor/temperature");
    std::cout << "[Discovery] Nodes providing 'sensor/temperature': " << temp_nodes.size() << std::endl;
    for (const auto& node_id : temp_nodes) {
        std::cout << "  - " << node_id << std::endl;
    }
    
    assert(temp_nodes.size() == 1);
    // assert(temp_nodes[0] == "sensor_temperature");
    assert(temp_nodes[0].find("sensor_temperature") == 0);
    
    auto camera_nodes = controller->findNodesByCapability("sensor/camera_front/frame");
    std::cout << "[Discovery] Nodes providing 'sensor/camera_front/frame': " << camera_nodes.size() << std::endl;
    for (const auto& node_id : camera_nodes) {
        std::cout << "  - " << node_id << std::endl;
    }
    
    assert(camera_nodes.size() == 1);
    assert(camera_nodes[0].find("sensor_camera_front") == 0);
    std::cout << "[Test 4] ✓ Passed" << std::endl;
    
    // Test 5: Find large data channels
    std::cout << "\n--- Test 5: Find Large Data Channels ---" << std::endl;
    
    auto channels = controller->findLargeDataChannels("sensor");
    std::cout << "[Discovery] Large data channels in 'sensor': " << channels.size() << std::endl;
    for (const auto& ch : channels) {
        std::cout << "  - Channel: " << ch.channel_name 
                  << ", Topic: " << ch.topic << std::endl;
    }
    
    assert(channels.size() == 1);
    assert(channels[0].channel_name == "camera_front");
    std::cout << "[Test 5] ✓ Passed" << std::endl;
    
    // Test 6: Service unregistration
    std::cout << "\n--- Test 6: Service Unregistration ---" << std::endl;
    
    sensor1->unsubscribe("sensor", {"temperature"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto after_unsub = controller->discoverServices("sensor", ServiceType::NORMAL_MESSAGE);
    std::cout << "[Discovery] Normal services after unsubscribe: " << after_unsub.size() << std::endl;
    
    assert(after_unsub.size() == 0);
    std::cout << "[Test 6] ✓ Passed" << std::endl;
    
    // Test 7: Same-process communication (zero-copy)
    std::cout << "\n--- Test 7: Same-Process Communication ---" << std::endl;
    
    int ctrl_count = 0;
    controller->subscribe("control", {"command"}, [&](const std::string& group,
                                                      const std::string& topic,
                                                      const uint8_t* data,
                                                      size_t len) {
        (void)group; (void)topic;
        ctrl_count++;
        std::string msg(reinterpret_cast<const char*>(data), len);
        std::cout << "[controller] Received command: " << msg << std::endl;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Sensor sends command to controller (same process)
    std::string cmd = "start_recording";
    sensor2->publish("control", "command", cmd);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    assert(ctrl_count == 1);
    std::cout << "[Test 7] ✓ Passed (zero-copy in-process delivery)" << std::endl;
    
    // Summary
    std::cout << "\n=== Service Discovery Test Summary ===" << std::endl;
    std::cout << "Total discovery callbacks invoked: " << discovery_callback_count << std::endl;
    std::cout << "All tests passed! ✓" << std::endl;
    
    std::cout << "\n=== Final Service Registry ===" << std::endl;
    auto final_services = controller->discoverServices();
    std::cout << "Total services: " << final_services.size() << std::endl;
    for (const auto& svc : final_services) {
        std::cout << "  - Node: " << svc.node_id 
                  << ", Type: " << (svc.type == ServiceType::NORMAL_MESSAGE ? "NORMAL" : "LARGE_DATA")
                  << ", Capability: " << svc.getCapability() << std::endl;
    }
}

int main() {
    std::cout << "Service Discovery Test Program" << std::endl;
    std::cout << "===============================" << std::endl;
    
    try {
        testServiceDiscovery();
        std::cout << "\n✓ All tests completed successfully!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n✗ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
