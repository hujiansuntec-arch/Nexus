#include "nexus/transport/UdpTransport.h"
#include "nexus/utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

namespace Nexus {
namespace rpc {

UdpTransport::UdpTransport()
    : socket_fd_(-1)
    , port_(0)
    , initialized_(false)
    , running_(false) {
}

UdpTransport::~UdpTransport() {
    shutdown();
}

bool UdpTransport::initialize(uint16_t port) {
    if (initialized_) {
        return true;
    }
    
    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        return false;
    }
    
    // Enable broadcast
    int broadcast_enable = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_BROADCAST, 
                   &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    // Don't allow address/port reuse for inter-process isolation
    // SO_REUSEADDR can allow multiple processes to bind to the same port on some systems
    // We want each node to have a unique port for proper identification
    /*
    int reuse = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, 
                   &reuse, sizeof(reuse)) < 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    */
    
    // Don't allow port reuse - each process should get its own port
    // This ensures proper process isolation
    /*
#ifdef SO_REUSEPORT
    int reuse_port = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEPORT, 
               &reuse_port, sizeof(reuse_port));
    // Don't fail if SO_REUSEPORT is not available
#endif
    */
    
    // Bind to port
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    // Get actual port (if auto-selected)
    socklen_t addr_len = sizeof(addr);
    if (getsockname(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0) {
        port_ = ntohs(addr.sin_port);
    } else {
        port_ = port;
    }
    
    // Set non-blocking
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
    
    // Start receive thread
    initialized_ = true;
    running_ = true;
    receive_thread_ = std::thread(&UdpTransport::receiveThread, this);
    
    return true;
}

void UdpTransport::shutdown() {
    if (!initialized_) {
        return;
    }
    
    running_ = false;
    
    // Wait for receive thread
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    
    // Close socket
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    
    initialized_ = false;
}

bool UdpTransport::send(const uint8_t* data, size_t size, 
                       const std::string& dest_addr, 
                       uint16_t dest_port) {
    if (!initialized_ || socket_fd_ < 0 || !data || size == 0) {
        return false;
    }
    
    // Determine destination
    const std::string& addr = dest_addr.empty() ? BROADCAST_ADDR : dest_addr;
    const uint16_t port = (dest_port == 0) ? DEFAULT_BROADCAST_PORT : dest_port;
    
    // Setup destination address
    struct sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    
    if (inet_pton(AF_INET, addr.c_str(), &dest.sin_addr) <= 0) {
        return false;
    }
    
    // Send data
    const ssize_t sent = sendto(socket_fd_, data, size, 0,
                         reinterpret_cast<struct sockaddr*>(&dest), 
                         sizeof(dest));
    
    return sent == static_cast<ssize_t>(size);
}

bool UdpTransport::broadcast(const uint8_t* data, size_t size) {
    return send(data, size, BROADCAST_ADDR, DEFAULT_BROADCAST_PORT);
}

void UdpTransport::setReceiveCallback(ReceiveCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    receive_callback_ = std::move(callback);
}

void UdpTransport::receiveThread() {
    constexpr size_t BUFFER_SIZE = 65536;
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    while (running_) {
        // Receive data
        ssize_t received = recvfrom(socket_fd_, buffer.data(), buffer.size(), 0,
                                   reinterpret_cast<struct sockaddr*>(&from_addr),
                                   &from_len);
        
        if (received > 0) {
            // Convert address to string
            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from_addr.sin_addr, addr_str, sizeof(addr_str));
            std::string from_address(addr_str);
            
            // Invoke callback
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                if (receive_callback_) {
                    NEXUS_DEBUG("Udp") << "UDP Received message from " << from_address 
                              << " (" << received << " bytes)";
                    receive_callback_(buffer.data(), static_cast<size_t>(received), from_address);
                }
            }
        } else if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                // Error
                break;
            }
        }
    }
}

} // namespace rpc
} // namespace Nexus
