#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

namespace librpc {

/**
 * @brief UDP transport for inter-process communication
 */
class UdpTransport {
public:
    using ReceiveCallback = std::function<void(const uint8_t* data, size_t size, 
                                              const std::string& from_addr)>;

    UdpTransport();
    ~UdpTransport();

    /**
     * @brief Initialize UDP socket
     * @param port Port to bind (0 = auto-select)
     * @return true if successful
     */
    bool initialize(uint16_t port = 0);

    /**
     * @brief Shutdown UDP socket
     */
    void shutdown();

    /**
     * @brief Send data via UDP
     * @param data Data buffer
     * @param size Data size
     * @param dest_addr Destination address (empty = broadcast)
     * @param dest_port Destination port (0 = use default broadcast port)
     * @return true if successful
     */
    bool send(const uint8_t* data, size_t size, 
             const std::string& dest_addr = "", 
             uint16_t dest_port = 0);

    /**
     * @brief Broadcast data to all nodes
     * @param data Data buffer
     * @param size Data size
     * @return true if successful
     */
    bool broadcast(const uint8_t* data, size_t size);

    /**
     * @brief Set callback for received data
     * @param callback Callback function
     */
    void setReceiveCallback(ReceiveCallback callback);

    /**
     * @brief Get bound port
     * @return Port number
     */
    uint16_t getPort() const { return port_; }

    /**
     * @brief Check if initialized
     * @return true if initialized
     */
    bool isInitialized() const { return initialized_; }

private:
    void receiveThread();
    
    int socket_fd_;
    uint16_t port_;
    std::atomic<bool> initialized_;
    std::atomic<bool> running_;
    
    std::thread receive_thread_;
    ReceiveCallback receive_callback_;
    std::mutex callback_mutex_;
    
    static constexpr uint16_t DEFAULT_BROADCAST_PORT = 47120;
    static constexpr const char* BROADCAST_ADDR = "255.255.255.255";
};

} // namespace librpc
