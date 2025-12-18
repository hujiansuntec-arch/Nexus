#include "nexus/transport/SharedMemoryTransportV3.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>    // 🔧 POSIX线程支持（Condition Variable）
#include <signal.h>     // For kill() process detection
#include <sys/epoll.h>  // 🔧 epoll支持（FIFO模式）
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>  // For errno
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sstream>  // For std::ostringstream
#include <thread>   // For std::this_thread::sleep_for

#include "nexus/core/NodeImpl.h"  // For handleNodeEvent callback
#include "nexus/utils/Logger.h"

// Static member definitions for C++14 compatibility
constexpr size_t Nexus::rpc::SharedMemoryTransportV3::QUEUE_CAPACITY;
constexpr size_t Nexus::rpc::SharedMemoryTransportV3::MAX_INBOUND_QUEUES;
constexpr int Nexus::rpc::SharedMemoryTransportV3::NodeHeader::MAX_ACCESSORS;
constexpr uint32_t Nexus::rpc::SharedMemoryTransportV3::MAGIC;
constexpr uint32_t Nexus::rpc::SharedMemoryTransportV3::VERSION;
constexpr uint64_t Nexus::rpc::SharedMemoryTransportV3::HEARTBEAT_INTERVAL_MS;
constexpr uint64_t Nexus::rpc::SharedMemoryTransportV3::NODE_TIMEOUT_MS;
constexpr uint64_t Nexus::rpc::SharedMemoryTransportV3::QUEUE_TIMEOUT_MS;


// QNX specific includes
#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <sys/procfs.h>
#endif

// ============ SharedMemoryTransportV3 Constants ============
// Flow control and congestion management
#define SHM_BACKOFF_BASE_US 10      // Congestion backoff base (microseconds)
#define SHM_BACKOFF_MAX_US 1000     // Congestion backoff maximum
#define SHM_CONGESTION_INCREMENT 5  // Congestion level increment on failure
#define SHM_CONGESTION_DECREMENT 1  // Congestion level decrement on success
#define SHM_CONGESTION_MAX 100      // Maximum congestion level
#define SHM_CONGESTION_INITIAL 10   // Initial congestion level on first failure

// Queue management and polling
#define SHM_QUEUE_REFRESH_INTERVAL \
    10  // Queue list refresh interval (loop iterations) - reduced from 200 for faster new queue detection
#define SHM_EMPTY_LOOP_THRESHOLD_SHORT 3  // Threshold for short timeout
#define SHM_EMPTY_LOOP_THRESHOLD_LONG 10  // Threshold for long timeout

// Timeout strategies (milliseconds)
#define SHM_TIMEOUT_SHORT_MS 1  // Short timeout (active receiving) - 降低到1ms for faster control message processing
#define SHM_TIMEOUT_MEDIUM_MS 5  // Medium timeout - 降低from 20ms
#define SHM_TIMEOUT_LONG_MS 10   // Long timeout (idle state) - 降低from 50ms
#define SHM_TIMEOUT_IDLE_MS 100  // Idle wait when no queues available
#define SHM_IDLE_SLEEP_MS 10     // Sleep time when idle

// Adaptive polling thresholds
#define SHM_ADAPTIVE_THRESHOLD 50  // Threshold for adaptive timeout switch

// Node discovery and initialization
#define SHM_STARTUP_WAIT_MS 300  // Wait time for remote nodes to respond (queryRemoteServices)

namespace Nexus {
namespace rpc {

// 🔧 辅助函数：判断消息是否为控制消息（需要高优先级处理）
static inline bool isControlMessage(const uint8_t* data, size_t size) {
    if (size < sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t)) {
        return false;
    }

    // 跳过magic(4) + version(2)，读取msg_type(1)
    uint8_t msg_type = data[6];

    // 控制消息类型：SUBSCRIBE(1), UNSUBSCRIBE(2), QUERY_SUBSCRIPTIONS(3),
    // SUBSCRIPTION_REPLY(4), SERVICE_REGISTER(5), SERVICE_UNREGISTER(6),
    // NODE_JOIN(7), NODE_LEAVE(8), HEARTBEAT(9)
    // 数据消息：DATA(0)
    return msg_type != 0;  // 非DATA消息都是控制消息
}

SharedMemoryTransportV3::SharedMemoryTransportV3()
    : initialized_(false),
      notify_mechanism_(NotifyMechanism::CONDITION_VARIABLE)  // Must match declaration order in header
      ,
      node_impl_(nullptr),
      my_shm_ptr_(nullptr),
      my_shm_fd_(-1),
      my_shm_(nullptr),
      receiving_(false) {}

SharedMemoryTransportV3::~SharedMemoryTransportV3() {
    stopReceiving();

    // 🔧 CRITICAL: 先收集需要清理的资源，再在锁外清理
    // 避免在持有锁时进行系统调用，减少死锁和内存损坏的风险
    std::vector<std::pair<void*, int>> resources_to_cleanup;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        resources_to_cleanup.reserve(remote_connections_.size());

        for (auto& pair : remote_connections_) {
            NEXUS_DEBUG("SHM-V3") << "Marking remote node for cleanup: " << pair.first;

            // 先置空 my_queue 指针
            pair.second.my_queue = nullptr;

            // 收集需要清理的资源
            void* shm_ptr = pair.second.shm_ptr;
            int shm_fd = pair.second.shm_fd;

            if (shm_ptr && shm_ptr != MAP_FAILED) {
                resources_to_cleanup.push_back({shm_ptr, shm_fd});
                pair.second.shm_ptr = nullptr;
                pair.second.shm_fd = -1;
            } else if (shm_fd >= 0) {
                resources_to_cleanup.push_back({nullptr, shm_fd});
                pair.second.shm_fd = -1;
            }

            pair.second.connected = false;
        }

        // 清空 map（在锁内，但不进行系统调用）
        remote_connections_.clear();
    }

    // CRITICAL: Remove my PID from all remote nodes' accessor lists
    // Do this BEFORE munmap to ensure atomic visibility
    pid_t my_pid = getpid();
    for (const auto& res : resources_to_cleanup) {
        if (res.first && res.first != MAP_FAILED) {
            NodeSharedMemory* remote_shm = static_cast<NodeSharedMemory*>(res.first);

            // Remove my PID from accessor list
            for (int i = 0; i < NodeHeader::MAX_ACCESSORS; ++i) {
                int32_t expected = static_cast<int32_t>(my_pid);
                if (remote_shm->header.accessor_pids[i].compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                                                std::memory_order_acquire)) {
                    remote_shm->header.num_accessors.fetch_sub(1, std::memory_order_release);
                    NEXUS_DEBUG("SHM-V3")
                        << "Removed PID " << my_pid << " from remote node accessor list (slot " << i << ")";
                    break;
                }
            }
        }
    }

    // Now safe to munmap and close
    for (const auto& res : resources_to_cleanup) {
        if (res.first && res.first != MAP_FAILED) {
            munmap(res.first, sizeof(NodeSharedMemory));
        }
        if (res.second >= 0) {
            close(res.second);
        }
    }
    NEXUS_DEBUG("SHM-V3") << "Disconnected from all remote nodes";

    // 移除自己的 accessor PID
    NEXUS_DEBUG("SHM-V3") << "Destructor: my_shm_=" << (void*)my_shm_ << ", my_shm_ptr_=" << (void*)my_shm_ptr_
                          << ", initialized_=" << initialized_;

    if (my_shm_ && my_shm_ptr_ && my_shm_ptr_ != MAP_FAILED) {
        removeAccessor(getpid());
        NEXUS_INFO("SHM-V3") << "Removed accessor PID " << getpid() << " from node " << node_id_;
    } else {
        NEXUS_WARN("SHM-V3") << "Cannot remove accessor PID from node " << node_id_ << ": "
                             << "my_shm_=" << (void*)my_shm_ << ", my_shm_ptr_=" << (void*)my_shm_ptr_;
    }

    // Unregister from registry FIRST (prevents new connections)
    if (initialized_) {
        NEXUS_DEBUG("SHM-V3") << "Unregistering node from registry: " << node_id_;
        registry_.unregisterNode(node_id_);
    }

    // Strategy: Delayed cleanup after grace period
    // 1. Remove accessor PID (already done above)
    // 2. Sleep briefly to let other nodes detect and disconnect
    // 3. Then unlink - safe because:
    //    - Not in registry anymore (no new connections)
    //    - Other nodes had time to disconnect
    //    - Existing mmaps continue to work

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bool has_unlink = false;
    // Now safe to unlink: not in registry, grace period passed
    if (my_shm_ && my_shm_name_.size() > 0) {
        if (!hasActiveAccessors(&my_shm_->header)) {
            has_unlink = true;
        }
    }
    if (my_shm_ptr_ && my_shm_ptr_ != MAP_FAILED) {
        munmap(my_shm_ptr_, sizeof(NodeSharedMemory));
        my_shm_ptr_ = nullptr;
        my_shm_ = nullptr;
    }
    if (my_shm_fd_ >= 0) {
        close(my_shm_fd_);
        my_shm_fd_ = -1;
    }
    if (has_unlink) {
        if (shm_unlink(my_shm_name_.c_str()) == 0) {
            NEXUS_DEBUG("SHM-V3") << "Unlinked shared memory: " << my_shm_name_;
        } else {
            NEXUS_WARN("SHM-V3") << "Failed to unlink " << my_shm_name_ << ": " << strerror(errno);
        }
    }

    NEXUS_DEBUG("SHM-V3") << "Node " << node_id_ << " destroyed";
}

bool SharedMemoryTransportV3::initialize(const std::string& node_id, const Config& config) {
    if (initialized_) {
        return true;
    }

    if (node_id.empty()) {
        NEXUS_ERROR("SHM-V3") << "Invalid node ID";
        return false;
    }

    // 验证配置
    if (config.max_inbound_queues > MAX_INBOUND_QUEUES) {
        NEXUS_ERROR("SHM-V3") << "Invalid config: max_inbound_queues (" << config.max_inbound_queues
                              << ") exceeds limit (" << MAX_INBOUND_QUEUES << ")";
        return false;
    }

    node_id_ = node_id;
    config_ = config;
    notify_mechanism_ = config.notify_mechanism;  // 保存通知机制配置

    // Initialize registry
    if (!registry_.initialize()) {
        NEXUS_ERROR("SHM-V3") << "Failed to initialize registry";
        return false;
    }

    // Generate unique shared memory name
    my_shm_name_ = generateShmName();

    // Create my shared memory
    if (!createMySharedMemory()) {
        NEXUS_ERROR("SHM-V3") << "Failed to create shared memory";
        return false;
    }

    // Register in registry
    if (!registry_.registerNode(node_id_, my_shm_name_)) {
        NEXUS_ERROR("SHM-V3") << "Failed to register node";
        destroyMySharedMemory();
        return false;
    }

    // 🔧 两阶段提交：设置ready标志
    my_shm_->header.ready.store(true, std::memory_order_release);

    initialized_ = true;

    const char* mechanism_name = "Unknown";
    if (notify_mechanism_ == NotifyMechanism::CONDITION_VARIABLE) {
        mechanism_name = "Condition Variable";
    } else if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
        mechanism_name = "POSIX Semaphore";
    } else if (notify_mechanism_ == NotifyMechanism::SMART_POLLING) {
        mechanism_name = "Smart Polling";
    }

    NEXUS_INFO("SHM-V3") << "Node " << node_id_ << " initialized"
                          << "\n  Notify mechanism: " << mechanism_name << "\n  Shared memory: " << my_shm_name_
                          << "\n  Queue capacity: " << config_.queue_capacity
                          << "\n  Max inbound queues: " << MAX_INBOUND_QUEUES;

    return true;
}

bool SharedMemoryTransportV3::send(const std::string& dest_node_id, const uint8_t* data, size_t size) {
    if (!initialized_) {
        return false;
    }

    if (dest_node_id == node_id_) {
        return false;  // Don't send to self
    }

    // 🔧 双队列架构：根据消息类型选择队列
    bool is_control = isControlMessage(data, size);

    // Fast path: check if already connected
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = remote_connections_.find(dest_node_id);
        if (it != remote_connections_.end() && it->second.connected && it->second.my_queue) {
            InboundQueue* queue = it->second.my_queue;

            // 🔧 单CV方案：统一的mutex/cond_var，根据消息类型选择队列
            LockFreeRingBuffer<256>* target_queue;
            sem_t* target_sem;
            std::atomic<uint32_t>* target_pending;

            if (is_control) {
                target_queue = reinterpret_cast<LockFreeRingBuffer<256>*>(&queue->control_queue);
                target_sem = &queue->control_sem;
                target_pending = &queue->control_pending;
            } else {
                target_queue = &queue->data_queue;
                target_sem = &queue->data_sem;
                target_pending = &queue->data_pending;
            }

            // 🔧 流控：检查拥塞等级（仅数据队列）
            if (!is_control) {
                uint32_t congestion = queue->congestion_level.load(std::memory_order_relaxed);
                if (congestion > 0 && congestion <= SHM_CONGESTION_MAX) {
                    int backoff_us = static_cast<int>(congestion) * SHM_BACKOFF_BASE_US;
                    if (backoff_us > 0 && backoff_us <= SHM_BACKOFF_MAX_US) {
                        std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                    }
                }
            }

            // 尝试发送
            bool success = target_queue->tryWrite(node_id_.c_str(), data, size);
            if (success) {
                stats_messages_sent_++;
                stats_bytes_sent_ += size;

                // 🔧 统一CV通知：无论control还是data都signal同一个cond_var
                if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
                    uint32_t prev = target_pending->fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        sem_post(target_sem);
                    }
                } else {
                    uint32_t prev = target_pending->fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        // 🔧 全局CV：从queue指针计算remote_shm地址
                        NodeSharedMemory* remote_shm = reinterpret_cast<NodeSharedMemory*>(
                            reinterpret_cast<char*>(queue) - offsetof(NodeSharedMemory, queues));
                        pthread_cond_signal(&remote_shm->header.global_cond);
                    }
                }

                // 🔧 流控：成功发送，降低拥塞等级（仅数据队列）
                if (!is_control) {
                    uint32_t congestion = queue->congestion_level.load(std::memory_order_relaxed);
                    if (congestion > 0) {
                        queue->congestion_level.fetch_sub(SHM_CONGESTION_DECREMENT, std::memory_order_relaxed);
                    }
                }
            } else {
                stats_messages_dropped_++;

                // 🔧 流控：发送失败，提高拥塞等级（仅数据队列）
                if (!is_control) {
                    queue->drop_count.fetch_add(1, std::memory_order_relaxed);
                    uint32_t congestion = queue->congestion_level.load(std::memory_order_relaxed);
                    if (congestion < SHM_CONGESTION_MAX) {
                        queue->congestion_level.fetch_add(SHM_CONGESTION_INCREMENT, std::memory_order_relaxed);
                    }
                }
            }
            return success;
        }
    }

    // Slow path: establish connection first
    NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Not connected to " << dest_node_id
                          << ", attempting lazy connection...";

    if (!connectToNode(dest_node_id)) {
        stats_messages_dropped_++;
        NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Failed to connect to " << dest_node_id;
        return false;
    }

    NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Successfully connected to " << dest_node_id;

    // Retry send after connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = remote_connections_.find(dest_node_id);
        if (it != remote_connections_.end() && it->second.connected && it->second.my_queue) {
            InboundQueue* queue = it->second.my_queue;

            // 🔧 单CV方案：统一的cond_var，根据消息类型选择队列
            LockFreeRingBuffer<256>* target_queue;
            sem_t* target_sem;
            std::atomic<uint32_t>* target_pending;

            if (is_control) {
                target_queue = reinterpret_cast<LockFreeRingBuffer<256>*>(&queue->control_queue);
                target_sem = &queue->control_sem;
                target_pending = &queue->control_pending;
            } else {
                target_queue = &queue->data_queue;
                target_sem = &queue->data_sem;
                target_pending = &queue->data_pending;
            }

            bool success = target_queue->tryWrite(node_id_.c_str(), data, size);
            if (success) {
                stats_messages_sent_++;
                stats_bytes_sent_ += size;

                // 🔧 统一CV通知：无论control还是data都signal同一个cond_var
                if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
                    uint32_t prev = target_pending->fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        sem_post(target_sem);
                    }
                } else {
                    uint32_t prev = target_pending->fetch_add(1, std::memory_order_release);
                    if (prev == 0) {
                        // 🔧 全局CV：signal远程节点的global_cond
                        NodeSharedMemory* remote_shm = reinterpret_cast<NodeSharedMemory*>(
                            reinterpret_cast<char*>(queue) - offsetof(NodeSharedMemory, queues));
                        pthread_cond_signal(&remote_shm->header.global_cond);
                    }
                }
            } else {
                stats_messages_dropped_++;
                if (!is_control) {
                    queue->drop_count.fetch_add(1, std::memory_order_relaxed);
                    queue->congestion_level.store(SHM_CONGESTION_INITIAL, std::memory_order_relaxed);
                }
            }
            return success;
        }
    }

    stats_messages_dropped_++;
    return false;
}

int SharedMemoryTransportV3::broadcast(const uint8_t* data, size_t size) {
    if (!initialized_) {
        return 0;
    }

    // Get all nodes from registry
    std::vector<NodeInfo> nodes = registry_.getAllNodes();
    int sent_count = 0;

    NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Broadcasting to " << nodes.size() << " nodes in registry";

    for (const auto& node : nodes) {
        if (node.node_id == node_id_) {
            continue;  // Skip self
        }

        NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Broadcast attempt to " << node.node_id
                              << " (active: " << node.active << ")";

        if (node.active && send(node.node_id, data, size)) {
            sent_count++;
            NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Broadcast to " << node.node_id << " SUCCESS";
        } else {
            NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Broadcast to " << node.node_id << " FAILED";
        }
    }

    return sent_count;
}

void SharedMemoryTransportV3::setReceiveCallback(ReceiveCallback callback) {
    receive_callback_ = callback;
}

void SharedMemoryTransportV3::startReceiving() {
    if (receiving_.load()) {
        return;
    }

    receiving_.store(true);

    // 🔧 立即更新心跳，防止在心跳线程启动前被误判超时
    registry_.updateHeartbeat(node_id_);

    // Start receive thread
    receive_thread_ = std::thread([this]() { receiveLoop(); });

    // Start heartbeat thread
    heartbeat_thread_ = std::thread([this]() { heartbeatLoop(); });

    NEXUS_DEBUG("SHM-V3") << "Started receiving threads for " << node_id_;
}

void SharedMemoryTransportV3::stopReceiving() {
    if (!receiving_.load()) {
        return;
    }

    NEXUS_DEBUG("SHM-V3") << "Stopping receiving threads for " << node_id_;

    // 🔧 使用 release 语义确保线程能看到变化
    receiving_.store(false, std::memory_order_release);

    // 🔧 Wake up all threads waiting on condition variables
    // This ensures receive threads exit immediately instead of waiting for timeout

    // 1. Wake up thread waiting for queue availability
    {
        std::lock_guard<std::mutex> lock(queue_wait_mutex_);
        queue_wait_cv_.notify_all();
    }

    // 2. Wake up threads waiting on queue-specific condition variables
    // 🔧 使用 trylock 避免永久阻塞
    if (my_shm_ && my_shm_ptr_ && my_shm_ptr_ != MAP_FAILED) {
        int ret = pthread_mutex_trylock(&my_shm_->header.global_mutex);
        if (ret == 0) {
            // 成功获取锁，广播条件变量
            pthread_cond_broadcast(&my_shm_->header.global_cond);
            pthread_mutex_unlock(&my_shm_->header.global_mutex);
            NEXUS_DEBUG("SHM-V3") << "Broadcasted to global_cond";
        } else {
            // 无法获取锁，可能有线程正在使用或锁已损坏
            NEXUS_WARN("SHM-V3") << "Cannot lock global_mutex during shutdown (errno=" << ret
                                 << "), threads may take longer to exit";
        }
    }

    NEXUS_DEBUG("SHM-V3") << "Waiting for receive thread to join...";
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    NEXUS_DEBUG("SHM-V3") << "Receive thread joined";

    NEXUS_DEBUG("SHM-V3") << "Waiting for heartbeat thread to join...";
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    NEXUS_DEBUG("SHM-V3") << "Heartbeat thread joined";

    // 🔧 CRITICAL: 线程已退出，现在可以安全清空 callback
    // 必须在线程 join 之后，确保没有线程还在访问
    receive_callback_ = nullptr;

    NEXUS_DEBUG("SHM-V3") << "Stopped receiving threads for " << node_id_;
}

std::vector<std::string> SharedMemoryTransportV3::getLocalNodes() const {
    std::vector<std::string> node_ids;

    if (!initialized_) {
        return node_ids;
    }

    std::vector<NodeInfo> nodes = registry_.getAllNodes();
    for (const auto& node : nodes) {
        if (node.active) {
            node_ids.push_back(node.node_id);
        }
    }

    return node_ids;
}

bool SharedMemoryTransportV3::isLocalNode(const std::string& node_id) const {
    if (!initialized_) {
        return false;
    }

    return registry_.nodeExists(node_id);
}

int SharedMemoryTransportV3::getConnectionCount() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    int count = 0;
    for (const auto& pair : remote_connections_) {
        if (pair.second.connected) {
            count++;
        }
    }
    return count;
}

void SharedMemoryTransportV3::warmupConnections() {
    if (!initialized_) {
        return;
    }

    std::vector<NodeInfo> nodes = registry_.getAllNodes();
    int connected = 0;

    for (const auto& node : nodes) {
        if (node.node_id != node_id_ && node.active) {
            if (connectToNode(node.node_id)) {
                connected++;
            }
        }
    }

    NEXUS_DEBUG("SHM-V3") << "Warmed up " << connected << " connections";
}

SharedMemoryTransportV3::TransportStats SharedMemoryTransportV3::getStats() const {
    TransportStats stats;
    stats.messages_sent = stats_messages_sent_.load();
    stats.messages_received = stats_messages_received_.load();
    stats.messages_dropped = stats_messages_dropped_.load();
    stats.bytes_sent = stats_bytes_sent_.load();
    stats.bytes_received = stats_bytes_received_.load();
    stats.active_connections = getConnectionCount();
    stats.inbound_queues = my_shm_ ? my_shm_->header.num_queues.load() : 0;

    // Calculate average queue depth
    stats.avg_queue_depth = 0.0;
    if (my_shm_) {
        uint32_t num_queues = my_shm_->header.num_queues.load();
        if (num_queues > 0) {
            uint64_t total_depth = 0;
            int active_queues = 0;
            for (uint32_t i = 0; i < num_queues && i < MAX_INBOUND_QUEUES; ++i) {
                InboundQueue& q = my_shm_->queues[i];
                // 🔧 Use acquire to ensure we see complete queue state
                if ((q.flags.load(std::memory_order_acquire) & 0x3) == 0x3) {
                    // 🔧 双队列架构：统计两个队列的深度
                    total_depth += q.control_queue.size() + q.data_queue.size();
                    active_queues++;
                }
            }
            if (active_queues > 0) {
                stats.avg_queue_depth = static_cast<double>(total_depth) / active_queues;
            }
        }
    }

    return stats;
}

bool SharedMemoryTransportV3::cleanupOrphanedMemory() {
    NEXUS_DEBUG("SHM-V3") << "Cleaning up orphaned shared memory...";

    size_t cleaned_count = 0;
    size_t total_freed = 0;

    // 辅助函数：检查进程是否存活
    auto isProcessAlive = [](int32_t pid) -> bool {
        if (pid <= 0) {
            return false;  // 无效PID
        }

        // kill(pid, 0) 不发送信号，只检查进程是否存在
        if (kill(pid, 0) == 0) {
            return true;  // 进程存在且有权限访问
        }

        // ESRCH: 进程不存在
        // EPERM: 进程存在但无权限（也算存活）
        return errno != ESRCH;
    };

// 扫描共享内存目录，查找librpc_node_*文件
// QNX uses /dev/shmem instead of /dev/shm
#ifdef __QNXNTO__
    const char* shm_dir = "/dev/shmem";
#else
    const char* shm_dir = "/dev/shm";
#endif

    DIR* dir = opendir(shm_dir);
    if (!dir) {
        NEXUS_ERROR("SHM-V3") << "Failed to open " << shm_dir << ": " << strerror(errno);
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // 只处理librpc_node_*的共享内存
        if (name.find("librpc_node_") != 0) {
            continue;
        }

        // 尝试打开共享内存
        std::string full_name = "/" + name;
        int fd = shm_open(full_name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            continue;
        }

        // 获取文件大小
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            continue;
        }

        size_t shm_size = st.st_size;

        // 映射共享内存以检查header
        void* addr = mmap(nullptr, sizeof(NodeHeader), PROT_READ, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            continue;
        }

        bool should_cleanup = false;
        std::string cleanup_reason;

        // 检查是否为有效的V3节点共享内存
        NodeHeader* header = static_cast<NodeHeader*>(addr);

        // Use acquire when reading remote process shared memory
        if (header->magic.load(std::memory_order_acquire) == MAGIC) {
            int32_t owner_pid = header->owner_pid.load(std::memory_order_acquire);

            // CRITICAL: Check if ALL accessors are dead using helper function
            // This is the safest cleanup condition
            bool owner_dead = (owner_pid > 0 && !isProcessAlive(owner_pid));
            bool has_active = hasActiveAccessors(header);

            // Cleanup conditions (ALL must be true):
            // 1. Owner is dead
            // 2. NO active accessor processes (no one has it mapped)
            if (owner_dead && !has_active) {
                uint32_t num_accessors = header->num_accessors.load(std::memory_order_acquire);
                should_cleanup = true;
                cleanup_reason = "owner and all " + std::to_string(num_accessors) + " accessor(s) dead";
            } else if (owner_dead && has_active) {
                // Owner dead but accessors still alive - don't cleanup yet
                uint32_t num_accessors = header->num_accessors.load(std::memory_order_acquire);
                NEXUS_DEBUG("SHM-V3") << "Skipping " << name << ": " << num_accessors << " accessor(s) still alive";
            }
        } else {
            // 不是有效的V3共享内存，可能是残留文件
            should_cleanup = true;
            cleanup_reason = "invalid magic number";
        }

        munmap(addr, sizeof(NodeHeader));
        close(fd);

        if (should_cleanup) {
            if (shm_unlink(full_name.c_str()) == 0) {
                cleaned_count++;
                total_freed += shm_size;
                NEXUS_DEBUG("SHM-V3") << "✓ Cleaned: " << name << " (" << (shm_size / 1024 / 1024) << " MB) - "
                                      << cleanup_reason;
            } else {
                NEXUS_ERROR("SHM-V3") << "✗ Failed to unlink " << name << ": " << strerror(errno);
            }
        }
    }

    closedir(dir);

    // 清理registry
    if (SharedMemoryRegistry::cleanupOrphanedRegistry()) {
        NEXUS_DEBUG("SHM-V3") << "✓ Cleaned orphaned registry";
    }

    if (cleaned_count > 0) {
        NEXUS_DEBUG("SHM-V3") << "Cleanup complete: removed " << cleaned_count << " node(s), freed "
                              << (total_freed / 1024 / 1024) << " MB";
    } else {
        NEXUS_DEBUG("SHM-V3") << "No orphaned nodes found";
    }

    return true;
}

int SharedMemoryTransportV3::getActiveNodeCount() {
    // Create temporary registry to query
    SharedMemoryRegistry temp_registry;
    if (!temp_registry.initialize()) {
        return -1;  // Registry doesn't exist
    }

    return temp_registry.getActiveNodeCount();
}

// Private helper methods

bool SharedMemoryTransportV3::createMySharedMemory() {
    // Create shared memory
    my_shm_fd_ = shm_open(my_shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (my_shm_fd_ < 0) {
        // Maybe old instance exists, try to clean up
        shm_unlink(my_shm_name_.c_str());
        my_shm_fd_ = shm_open(my_shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        if (my_shm_fd_ < 0) {
            NEXUS_ERROR("SHM-V3") << "Failed to create shared memory: " << strerror(errno);
            return false;
        }
    }

    // Set size
    size_t shm_size = sizeof(NodeSharedMemory);
    if (ftruncate(my_shm_fd_, shm_size) < 0) {
        NEXUS_ERROR("SHM-V3") << "Failed to set size: " << strerror(errno);
        close(my_shm_fd_);
        my_shm_fd_ = -1;
        shm_unlink(my_shm_name_.c_str());
        return false;
    }

// Map memory
// Note: MAP_NORESERVE may not be supported on QNX, use MAP_SHARED only
#ifdef __QNXNTO__
    my_shm_ptr_ = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, my_shm_fd_, 0);
#else
    my_shm_ptr_ = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, my_shm_fd_, 0);
#endif
    if (my_shm_ptr_ == MAP_FAILED) {
        NEXUS_ERROR("SHM-V3") << "Failed to map memory: " << strerror(errno);
        close(my_shm_fd_);
        my_shm_fd_ = -1;
        shm_unlink(my_shm_name_.c_str());
        return false;
    }

    my_shm_ = static_cast<NodeSharedMemory*>(my_shm_ptr_);

    // Initialize header
    my_shm_->header.magic.store(MAGIC, std::memory_order_relaxed);
    my_shm_->header.version.store(VERSION, std::memory_order_relaxed);
    my_shm_->header.num_queues.store(0, std::memory_order_relaxed);
    // Safe cast: max_inbound_queues is validated in initialize()
    my_shm_->header.max_queues.store(static_cast<uint32_t>(config_.max_inbound_queues), std::memory_order_relaxed);
    // Use system_clock for cross-process timestamp consistency
    my_shm_->header.last_heartbeat.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);
    my_shm_->header.ready.store(false, std::memory_order_relaxed);
    my_shm_->header.owner_pid.store(getpid(), std::memory_order_relaxed);

    // CRITICAL: Initialize accessor tracking
    my_shm_->header.num_accessors.store(0, std::memory_order_relaxed);
    for (int i = 0; i < NodeHeader::MAX_ACCESSORS; ++i) {
        my_shm_->header.accessor_pids[i].store(0, std::memory_order_relaxed);
    }

    // 🔧 初始化全局共享的pthread对象（所有队列共享同一个cond_var）
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&my_shm_->header.global_mutex, &mattr);
    pthread_cond_init(&my_shm_->header.global_cond, &cattr);

    pthread_mutexattr_destroy(&mattr);
    pthread_condattr_destroy(&cattr);

    // Initialize all queues
    for (size_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        my_shm_->queues[i].flags.store(0, std::memory_order_relaxed);
        // 🔧 Initialize atomic sender_id array
        for (int j = 0; j < 8; ++j) {
            my_shm_->queues[i].sender_id_atomic[j].store(0, std::memory_order_relaxed);
        }
        // 🔧 双队列架构：初始化两个pending计数器
        my_shm_->queues[i].control_pending.store(0, std::memory_order_relaxed);
        my_shm_->queues[i].data_pending.store(0, std::memory_order_relaxed);
        // 🔧 初始化流控字段
        my_shm_->queues[i].congestion_level.store(0, std::memory_order_relaxed);
        my_shm_->queues[i].drop_count.store(0, std::memory_order_relaxed);
    }

    size_t mb = shm_size / (1024 * 1024);
    NEXUS_DEBUG("SHM-V3") << "Created shared memory: " << my_shm_name_ << " (" << mb
                          << " MB, max_queues=" << config_.max_inbound_queues << ")";

    return true;
}

void SharedMemoryTransportV3::destroyMySharedMemory() {
    // 🔧 注意：pthread对象是跨进程共享的，不能随意destroy
    // pthread_mutex/cond_destroy只应该在最后一个使用者退出时调用
    // 这里我们只是unmap，让操作系统在shm被删除时清理

    if (my_shm_ptr_ && my_shm_ptr_ != MAP_FAILED) {
        munmap(my_shm_ptr_, sizeof(NodeSharedMemory));
        my_shm_ptr_ = nullptr;
        my_shm_ = nullptr;
    }

    if (my_shm_fd_ >= 0) {
        close(my_shm_fd_);
        my_shm_fd_ = -1;
    }

    if (!my_shm_name_.empty()) {
        shm_unlink(my_shm_name_.c_str());
        NEXUS_DEBUG("SHM-V3") << "Destroyed shared memory: " << my_shm_name_;
    }
}

bool SharedMemoryTransportV3::connectToNode(const std::string& target_node_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    // Check if already connected
    auto it = remote_connections_.find(target_node_id);
    if (it != remote_connections_.end() && it->second.connected) {
        return true;
    }

    // Get target node info from registry
    NodeInfo target_info;
    bool found = false;

    // 🔧 CRITICAL: Retry mechanism for cross-process registry lookup
    // Due to timing issues in shared memory synchronization, retry a few times
    for (int retry = 0; retry < 5 && !found; ++retry) {
        if (registry_.findNode(target_node_id, target_info)) {
            found = true;
            break;
        }
        if (retry < 4) {
            // Short delay before retry (microseconds)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    if (!found) {
        NEXUS_ERROR("SHM-V3") << "Node not found in registry: " << target_node_id;
        return false;
    }

    // Create connection structure
    RemoteConnection conn;
    conn.node_id = target_node_id;
    conn.shm_name = target_info.shm_name;

    // Open target node's shared memory
    conn.shm_fd = shm_open(target_info.shm_name.c_str(), O_RDWR, 0666);
    if (conn.shm_fd < 0) {
        NEXUS_ERROR("SHM-V3") << "Failed to open remote shm " << target_info.shm_name << ": " << strerror(errno);
        return false;
    }

// Map remote memory
// Note: MAP_NORESERVE may not be supported on QNX, use MAP_SHARED only
#ifdef __QNXNTO__
    conn.shm_ptr = mmap(nullptr, sizeof(NodeSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, conn.shm_fd, 0);
#else
    conn.shm_ptr =
        mmap(nullptr, sizeof(NodeSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, conn.shm_fd, 0);
#endif
    if (conn.shm_ptr == MAP_FAILED) {
        NEXUS_ERROR("SHM-V3") << "Failed to map remote shm: " << strerror(errno);
        close(conn.shm_fd);
        return false;
    }

    NodeSharedMemory* remote_shm = static_cast<NodeSharedMemory*>(conn.shm_ptr);

    // Verify magic
    uint32_t magic = remote_shm->header.magic.load(std::memory_order_acquire);
    if (magic != MAGIC) {
        NEXUS_ERROR("SHM-V3") << "Invalid magic in remote shm: 0x" << std::hex << magic;
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }

    // 🔧 两阶段提交：验证节点是否完全初始化
    bool is_ready = remote_shm->header.ready.load(std::memory_order_acquire);
    if (!is_ready) {
        // 节点正在初始化中，不是错误，直接返回 false 等待下次重试
        NEXUS_DEBUG("SHM-V3") << "Remote node not ready yet: " << target_node_id << ", will retry later";
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }

    // 健康检查: 验证进程是否存活
    // 注意：kill(pid, 0) 可能因权限问题失败，所以要谨慎处理
    pid_t owner_pid = remote_shm->header.owner_pid.load(std::memory_order_acquire);
    if (owner_pid > 0 && kill(owner_pid, 0) != 0) {
        // 进程不存在，不要连接
        NEXUS_WARN("SHM-V3") << "Remote node process is dead (pid=" << owner_pid
                             << "), skipping connection to: " << target_node_id;
        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        // 注意：不在这里删除共享内存文件，避免 Bus error
        // 其他节点可能正在访问，由心跳系统统一清理
        return false;
    }

    // CRITICAL: Add my PID to target node's accessor list
    // This tracks that I have opened and mapped this shared memory
    pid_t my_pid = getpid();
    bool added = false;
    for (int i = 0; i < NodeHeader::MAX_ACCESSORS; ++i) {
        int32_t expected = 0;
        if (remote_shm->header.accessor_pids[i].compare_exchange_strong(
                expected, static_cast<int32_t>(my_pid), std::memory_order_acq_rel, std::memory_order_acquire)) {
            remote_shm->header.num_accessors.fetch_add(1, std::memory_order_release);
            added = true;
            NEXUS_DEBUG("SHM-V3") << "Added PID " << my_pid << " to " << target_node_id << " accessor list (slot " << i
                                  << ")";
            break;
        }
    }

    if (!added) {
        NEXUS_WARN("SHM-V3") << "Accessor list full for " << target_node_id << ", connection may be unsafe to cleanup";
    }

    // Find or create queue for me in target node's memory
    conn.my_queue = findOrCreateQueue(remote_shm, node_id_);
    if (!conn.my_queue) {
        NEXUS_ERROR("SHM-V3") << "Failed to create queue in remote node";

        // Remove my PID if queue allocation failed
        if (added) {
            for (int i = 0; i < NodeHeader::MAX_ACCESSORS; ++i) {
                int32_t expected = static_cast<int32_t>(my_pid);
                if (remote_shm->header.accessor_pids[i].compare_exchange_strong(expected, 0,
                                                                                std::memory_order_acq_rel)) {
                    remote_shm->header.num_accessors.fetch_sub(1, std::memory_order_release);
                    break;
                }
            }
        }

        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
        close(conn.shm_fd);
        return false;
    }

    conn.connected = true;

    remote_connections_[target_node_id] = conn;

    NEXUS_INFO("SHM-V3") << "Connected to node: " << target_node_id << " (shm: " << target_info.shm_name << ")";

    return true;
}

void SharedMemoryTransportV3::disconnectFromNode(const std::string& target_node_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = remote_connections_.find(target_node_id);
    if (it == remote_connections_.end()) {
        return;
    }

    RemoteConnection& conn = it->second;

    // Remove our PID from the remote node's accessor list before disconnecting
    if (conn.shm_ptr && conn.shm_ptr != MAP_FAILED) {
        NodeSharedMemory* remote_shm = static_cast<NodeSharedMemory*>(conn.shm_ptr);
        removeAccessorFromNode(&remote_shm->header, getpid());

        munmap(conn.shm_ptr, sizeof(NodeSharedMemory));
    }

    if (conn.shm_fd >= 0) {
        close(conn.shm_fd);
    }

    remote_connections_.erase(it);

    NEXUS_DEBUG("SHM-V3") << "Disconnected from node: " << target_node_id;
}

SharedMemoryTransportV3::InboundQueue* SharedMemoryTransportV3::findOrCreateQueue(NodeSharedMemory* remote_shm,
                                                                                  const std::string& sender_id) {
    // First, try to find existing queue
    // 🔧 Use seq_cst to see all queues including just-created ones
    uint32_t num_queues = remote_shm->header.num_queues.load(std::memory_order_seq_cst);
    for (uint32_t i = 0; i < num_queues && i < MAX_INBOUND_QUEUES; ++i) {
        InboundQueue& q = remote_shm->queues[i];
        // 🔧 Use seq_cst to ensure we see complete queue initialization
        if (q.flags.load(std::memory_order_seq_cst) & 0x1) {
            // 🔧 Read sender_id atomically
            std::string existing_sender = SharedMemoryRegistry::readAtomicString(q.sender_id_atomic, 64);
            if (existing_sender == sender_id) {
                return &q;
            }
        }
    }

    // Not found, create new queue
    uint32_t max_queues = remote_shm->header.max_queues.load(std::memory_order_relaxed);
    if (num_queues >= max_queues) {
        NEXUS_ERROR("SHM-V3") << "Remote node queue limit reached (" << max_queues << ")";
        return nullptr;
    }

    // Find free slot
    for (size_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        InboundQueue& q = remote_shm->queues[i];
        uint32_t expected = 0;
        // 🔧 使用acquire-release语义的CAS保证原子性
        if (q.flags.compare_exchange_strong(expected, 0x1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Successfully claimed this slot (flags = 0x1: valid but not active yet)
            // 🔧 Write sender_id atomically
            SharedMemoryRegistry::writeAtomicString(q.sender_id_atomic, sender_id, 64);

            // 🔧 双队列架构：初始化控制队列和数据队列
            if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
                // Semaphore模式
                if (sem_init(&q.control_sem, 1, 0) != 0 || sem_init(&q.data_sem, 1, 0) != 0) {
                    NEXUS_ERROR("SHM-V3") << "sem_init failed: " << strerror(errno);
                    q.flags.store(0, std::memory_order_release);
                    return nullptr;
                }
                q.control_pending.store(0, std::memory_order_relaxed);
                q.data_pending.store(0, std::memory_order_relaxed);

                // 🔧 CRITICAL: CPU-level fence for cross-process visibility
                std::atomic_thread_fence(std::memory_order_release);

                // 🔧 Use seq_cst for cross-process immediate visibility
                q.flags.store(0x3, std::memory_order_seq_cst);  // valid | active

                NEXUS_DEBUG("SHM-V3") << "Created dual queues with Semaphore for sender: " << sender_id;
            } else {
                // Condition Variable模式：使用全局共享的cond_var
                q.control_pending.store(0, std::memory_order_relaxed);
                q.data_pending.store(0, std::memory_order_relaxed);
                q.congestion_level.store(0, std::memory_order_relaxed);
                q.drop_count.store(0, std::memory_order_relaxed);

                // 🔧 CRITICAL: CPU-level fence for cross-process visibility
                std::atomic_thread_fence(std::memory_order_release);

                // 🔧 Use seq_cst for cross-process immediate visibility
                q.flags.store(0x3, std::memory_order_seq_cst);  // valid | active

                NEXUS_DEBUG("SHM-V3") << "Created dual queues (global CV) for sender: " << sender_id
                                      << " (control: 64, data: 256)";

                // 🔧 立即通知远程节点有新队列
                pthread_mutex_lock(&remote_shm->header.global_mutex);
                pthread_cond_signal(&remote_shm->header.global_cond);
                pthread_mutex_unlock(&remote_shm->header.global_mutex);
            }

            // 🔧 Use seq_cst to ensure queue is visible before count increases
            remote_shm->header.num_queues.fetch_add(1, std::memory_order_seq_cst);

            NEXUS_DEBUG("SHM-V3") << "Created queue in remote node, num_queues now: "
                                  << remote_shm->header.num_queues.load();

            return &q;
        }
    }

    return nullptr;
}

void SharedMemoryTransportV3::receiveLoop() {
    if (notify_mechanism_ == NotifyMechanism::SEMAPHORE) {
        receiveLoop_Semaphore();
    } else {
        receiveLoop_CV();
    }
}

// Condition Variable模式的接收循环（优化版）
void SharedMemoryTransportV3::receiveLoop_CV() {
    NEXUS_DEBUG("SHM-V3") << "Receive loop started for " << node_id_ << " (Condition Variable mode - optimized)";

    static constexpr size_t MESSAGE_SIZE = 2048;
    uint8_t buffer[MESSAGE_SIZE];

    // 缓存活跃队列列表，减少遍历开销
    std::vector<InboundQueue*> active_queues;
    uint32_t cached_num_queues = 0;
    int queue_refresh_counter = 0;

    // 自适应超时：根据消息流量动态调整
    int consecutive_empty_loops = 0;

    while (receiving_.load()) {
        if (!my_shm_) {
            // 🔧 Wait for shared memory - use longer sleep since this is rare
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // ✅ 优化7: 减少队列刷新检查的开销
        // 只在必要时才检查 num_queues（减少 atomic load）
        queue_refresh_counter++;

        // 🔧 关键优化：立即检测num_queues变化，无需等待计数器
        // Use acquire to see queues created by other processes
        uint32_t current_num_queues = my_shm_->header.num_queues.load(std::memory_order_acquire);

        if (current_num_queues != cached_num_queues) {
             NEXUS_INFO("SHM-V3") << "Num queues changed: " << cached_num_queues << " -> " << current_num_queues;
        }

        // 延长刷新间隔（降低检查频率），但num_queues变化时立即刷新
        bool need_refresh = (current_num_queues != cached_num_queues) ||
                            (queue_refresh_counter >= SHM_QUEUE_REFRESH_INTERVAL) || active_queues.empty();

        if (need_refresh) {
            if (current_num_queues != cached_num_queues || active_queues.empty()) {
                active_queues.clear();
                active_queues.reserve(current_num_queues);  // ✅ 预留空间避免realloc

                for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
                    InboundQueue& q = my_shm_->queues[i];
                    // 🔧 Use acquire to see all queue fields initialized by sender
                    uint32_t flags = q.flags.load(std::memory_order_acquire);
                    if ((flags & 0x3) == 0x3) {
                        active_queues.push_back(&q);
                        NEXUS_INFO("SHM-V3") << "Added active queue " << i;
                    }
                }
                cached_num_queues = current_num_queues;

                // 🔧 如果检测到新队列，通知等待的线程
                if (!active_queues.empty() && !has_active_queues_.load(std::memory_order_relaxed)) {
                    has_active_queues_.store(true, std::memory_order_release);
                    queue_wait_cv_.notify_one();
                }
            }
            queue_refresh_counter = 0;
        }

        if (active_queues.empty()) {
            // 🔧 No queues available - use condition variable to wait efficiently
            // This avoids busy-wait and reduces CPU to 0% when idle
            has_active_queues_.store(false, std::memory_order_release);

            std::unique_lock<std::mutex> lock(queue_wait_mutex_);
            queue_wait_cv_.wait_for(lock, std::chrono::milliseconds(SHM_TIMEOUT_IDLE_MS),
                                    [this]() { return !receiving_.load() || has_active_queues_.load(); });
            continue;
        }

        // Mark that we have active queues
        has_active_queues_.store(true, std::memory_order_relaxed);

        // 🔧 双队列架构：优先处理控制队列，确保服务发现不受数据流量影响
        bool has_messages = false;

        // === 第一遍：优先处理所有队列的控制消息 ===
        for (auto* q : active_queues) {
            uint32_t control_pending = q->control_pending.load(std::memory_order_acquire);
            if (control_pending == 0) {
                continue;
            }

            // 🔧 Use acquire to ensure we see all queue fields
            uint32_t flags = q->flags.load(std::memory_order_acquire);
            if ((flags & 0x3) != 0x3) {
                continue;
            }

            // 处理控制队列的所有消息（控制消息少，全部处理）
            int processed = 0;
            while (true) {
                char from_node[64];
                size_t msg_size = MESSAGE_SIZE;

                // 🔧 直接使用control_queue（LockFreeRingBuffer<64>）
                if (!q->control_queue.tryRead(from_node, buffer, msg_size)) {
                    break;
                }

                processed++;
                has_messages = true;

                // 🔧 检查 callback 是否有效（可能在析构时被清空）
                auto callback = receive_callback_;
                if (callback) {
                    NEXUS_INFO("SHM-V3")
                        << "[CTRL] Received control message from " << from_node << " (" << msg_size << " bytes)";
                    callback(buffer, msg_size, from_node);
                } else {
                    NEXUS_WARN("SHM-V3") << "Callback is null!";
                }
            }

            if (processed > 0) {
                stats_messages_received_ += processed;
                q->control_pending.fetch_sub(processed, std::memory_order_release);
            }
        }

        // === 第二遍：处理数据队列（限流） ===
        for (auto* q : active_queues) {
            uint32_t data_pending = q->data_pending.load(std::memory_order_acquire);
            if (data_pending == 0) {
                continue;
            }

            // 🔧 Use acquire to ensure we see all queue fields
            uint32_t flags = q->flags.load(std::memory_order_acquire);
            if ((flags & 0x3) != 0x3) {
                continue;
            }

            // 限流：每轮每队列最多处理N条数据消息，避免阻塞下一轮控制消息处理
            int processed = 0;
            const int MAX_DATA_PER_QUEUE = 16;

            while (processed < MAX_DATA_PER_QUEUE) {
                char from_node[64];
                size_t msg_size = MESSAGE_SIZE;

                if (!q->data_queue.tryRead(from_node, buffer, msg_size)) {
                    break;
                }

                processed++;
                has_messages = true;

                // 🔧 检查 callback 是否有效（可能在析构时被清空）
                auto callback = receive_callback_;
                if (callback) {
                    callback(buffer, msg_size, from_node);
                }
            }

            if (processed > 0) {
                stats_messages_received_ += processed;
                q->data_pending.fetch_sub(processed, std::memory_order_release);
            }
        }

        // 🔧 步骤2：如果没有消息，使用condition variable等待
        if (!has_messages && !active_queues.empty()) {
            consecutive_empty_loops++;

            // 🔧 全局CV方案：所有队列共享my_shm_->header.global_cond
            // 任何队列收到消息都会signal这个统一的cond_var

            int timeout_ms;
            if (consecutive_empty_loops > SHM_EMPTY_LOOP_THRESHOLD_LONG) {
                timeout_ms = 100;  // 长时间空闲：100ms
            } else if (consecutive_empty_loops > SHM_EMPTY_LOOP_THRESHOLD_SHORT) {
                timeout_ms = 50;  // 中等空闲：50ms
            } else {
                timeout_ms = 10;  // 刚开始空闲：10ms
            }

            // 等待全局cond_var
            pthread_mutex_lock(&my_shm_->header.global_mutex);

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);

            long nsec_add = (long)timeout_ms * 1000000L;
            ts.tv_sec += nsec_add / 1000000000L;
            ts.tv_nsec += nsec_add % 1000000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }

            // 等待统一的global_cond，任何消息（control或data）都会唤醒
            pthread_cond_timedwait(&my_shm_->header.global_cond, &my_shm_->header.global_mutex, &ts);
            pthread_mutex_unlock(&my_shm_->header.global_mutex);

        } else {
            // 🔧 有消息时重置计数器，保持短超时以降低延迟
            consecutive_empty_loops = 0;
        }
    }

    NEXUS_DEBUG("SHM-V3") << "Receive loop stopped for " << node_id_ << " (CV mode)";
}

// 🔧 Semaphore模式的接收循环（优化版：真正利用sem_timedwait阻塞等待）
void SharedMemoryTransportV3::receiveLoop_Semaphore() {
    NEXUS_DEBUG("SHM-V3") << "Receive loop started for " << node_id_ << " (Semaphore mode - optimized)";

    static constexpr size_t MESSAGE_SIZE = 2048;
    uint8_t buffer[MESSAGE_SIZE];

    // 🔧 缓存活跃队列列表，定期更新以降低开销
    std::vector<InboundQueue*> active_queues;
    uint32_t cached_num_queues = 0;  // 缓存num_queues用于检测变化
    int queue_refresh_counter = 0;
    const int QUEUE_REFRESH_INTERVAL = SHM_QUEUE_REFRESH_INTERVAL;  // 定期刷新队列列表

    // 🔧 自适应超时：有消息时使用短超时，无消息时使用长超时
    int consecutive_empty_loops = 0;
    const int ADAPTIVE_THRESHOLD = SHM_ADAPTIVE_THRESHOLD;  // 空循环阈值后切换到长超时

    while (receiving_.load()) {
        if (!my_shm_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SHM_IDLE_SLEEP_MS));
            continue;
        }

        // 🔧 检测队列变化：num_queues变化或定期刷新
        // Use acquire to see queues created by other processes
        uint32_t current_num_queues = my_shm_->header.num_queues.load(std::memory_order_acquire);
        queue_refresh_counter++;

        if (current_num_queues != cached_num_queues || queue_refresh_counter >= QUEUE_REFRESH_INTERVAL ||
            active_queues.empty()) {
            active_queues.clear();
            for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
                InboundQueue& q = my_shm_->queues[i];
                // 🔧 Use acquire to see complete queue initialization
                uint32_t flags = q.flags.load(std::memory_order_acquire);
                if ((flags & 0x3) == 0x3) {
                    active_queues.push_back(&q);
                }
            }
            cached_num_queues = current_num_queues;
            queue_refresh_counter = 0;
        }

        if (active_queues.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SHM_IDLE_SLEEP_MS));
            continue;
        }

        // Mark that we have active queues
        has_active_queues_.store(true, std::memory_order_relaxed);

        // 🔧 双队列架构：优先处理控制队列
        bool has_messages = false;

        // === 第一遍：优先处理所有队列的控制消息 ===
        for (auto* q : active_queues) {
            // 🔧 Use acquire to ensure we see all queue fields
            uint32_t flags = q->flags.load(std::memory_order_acquire);
            if ((flags & 0x3) != 0x3) {
                continue;
            }

            int processed = 0;
            while (true) {
                char from_node[64];
                size_t msg_size = MESSAGE_SIZE;

                // 🔧 直接使用control_queue（LockFreeRingBuffer<64>）
                if (!q->control_queue.tryRead(from_node, buffer, msg_size)) {
                    break;
                }

                stats_messages_received_++;
                stats_bytes_received_ += msg_size;
                has_messages = true;
                processed++;

                // 🔧 检查 callback 是否有效（可能在析构时被清空）
                auto callback = receive_callback_;
                if (callback) {
                    NEXUS_DEBUG("SHM-V3") << "[CTRL] Received control message from " << from_node;
                    callback(buffer, msg_size, from_node);
                }
            }

            if (processed > 0) {
                q->control_pending.fetch_sub(processed, std::memory_order_release);
            }
        }

        // === 第二遍：处理数据队列（限流） ===
        for (auto* q : active_queues) {
            // 🔧 Use acquire to ensure we see all queue fields
            uint32_t flags = q->flags.load(std::memory_order_acquire);
            if ((flags & 0x3) != 0x3) {
                continue;
            }

            int processed = 0;
            const int MAX_DATA_PER_QUEUE = 16;

            while (processed < MAX_DATA_PER_QUEUE) {
                char from_node[64];
                size_t msg_size = MESSAGE_SIZE;

                if (!q->data_queue.tryRead(from_node, buffer, msg_size)) {
                    break;
                }

                stats_messages_received_++;
                stats_bytes_received_ += msg_size;
                has_messages = true;
                processed++;

                // 🔧 检查 callback 是否有效（可能在析构时被清空）
                auto callback = receive_callback_;
                if (callback) {
                    callback(buffer, msg_size, from_node);
                }
            }

            if (processed > 0) {
                q->data_pending.fetch_sub(processed, std::memory_order_release);
            }
        }

        // 🔧 步骤2：如果没有消息，使用sem_timedwait阻塞等待
        if (!has_messages && !active_queues.empty()) {
            consecutive_empty_loops++;

            int timeout_ms =
                (consecutive_empty_loops > ADAPTIVE_THRESHOLD) ? SHM_TIMEOUT_LONG_MS : SHM_TIMEOUT_SHORT_MS;

            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            long timeout_ns = timeout_ms * 1000000L;
            timeout.tv_nsec += timeout_ns;
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec++;
                timeout.tv_nsec -= 1000000000;
            }

            // 🔧 优先等待控制信号量
            bool got_signal = false;
            for (auto* q : active_queues) {
                if (sem_trywait(&q->control_sem) == 0) {
                    got_signal = true;
                    break;
                }
            }

            // 如果没有控制信号，尝试数据信号量
            if (!got_signal) {
                for (auto* q : active_queues) {
                    if (sem_trywait(&q->data_sem) == 0) {
                        got_signal = true;
                        break;
                    }
                }
            }

            // 如果都没有立即可用的信号，等待第一个队列的控制信号量
            if (!got_signal) {
                sem_timedwait(&active_queues[0]->control_sem, &timeout);
            }
        } else {
            // 有消息时重置空循环计数
            consecutive_empty_loops = 0;
        }
    }

    NEXUS_DEBUG("SHM-V3") << "Receive loop stopped for " << node_id_ << " (Semaphore mode)";
}

void SharedMemoryTransportV3::heartbeatLoop() {
    NEXUS_DEBUG("SHM-V3") << "Heartbeat loop started for " << node_id_;

    int heartbeat_count = 0;
    while (receiving_.load()) {
        // Update my heartbeat in registry
        bool updated = registry_.updateHeartbeat(node_id_);
        heartbeat_count++;

        // 🔧 每10次心跳打印一次日志，确保heartbeat loop没有卡住
        if (heartbeat_count % 10 == 0) {
            NEXUS_DEBUG("SHM-V3") << "Heartbeat #" << heartbeat_count << " for " << node_id_ << " (updated: " << updated
                                  << ")";
        }

        // Update my heartbeat in my shared memory
        if (my_shm_) {
            // 🔧 Use system_clock for cross-process timestamp consistency
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            // 🔧 使用release语义更新heartbeat，确保对其他进程可见
            my_shm_->header.last_heartbeat.store(ms.count(), std::memory_order_release);
        }

        // 🔧 CRITICAL: Periodically discover new nodes from registry
        // This ensures 100% reliability even when nodes start simultaneously
        // Solves race condition: node2 and node3 both register at ~same time
        {
            auto all_nodes = registry_.getAllNodes();
            std::vector<std::string> new_nodes;

            // Find new nodes (check outside of lock to avoid deadlock)
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                for (const auto& node : all_nodes) {
                    // Skip self
                    if (node.node_id == node_id_) {
                        continue;
                    }

                    // Skip if already connected
                    auto it = remote_connections_.find(node.node_id);
                    if (it != remote_connections_.end() && it->second.connected) {
                        continue;
                    }

                    // New node detected
                    new_nodes.push_back(node.node_id);
                }
            }

            // Connect to new nodes (outside of lock)
            for (const auto& new_node_id : new_nodes) {
                NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Discovered new node in registry: " << new_node_id
                                      << " - establishing connection";

                bool connected = connectToNode(new_node_id);

                if (connected) {
                    NEXUS_DEBUG("SHM-V3") << "[" << node_id_ << "] Successfully connected to new node: " << new_node_id;
                }
            }
        }

        // Get nodes before cleanup (for detecting removed nodes)
        std::vector<NodeInfo> nodes_before;
        if (node_impl_) {
            nodes_before = registry_.getAllNodes();
        }

        // Clean up stale nodes from registry
        int cleaned = registry_.cleanupStaleNodes(NODE_TIMEOUT_MS);

        // Notify NodeImpl about removed nodes (trigger NODE_LEFT events)
        if (cleaned > 0 && node_impl_) {
            auto nodes_after = registry_.getAllNodes();

            // Find which nodes were removed
            for (const auto& node : nodes_before) {
                // Skip self
                if (node.node_id == node_id_) {
                    continue;
                }

                // Check if node still exists
                bool found = false;
                for (const auto& n : nodes_after) {
                    if (n.node_id == node.node_id) {
                        found = true;
                        break;
                    }
                }

                // Node was removed - trigger NODE_LEFT event
                if (!found) {
                    NEXUS_DEBUG("SHM-V3")
                        << "Heartbeat timeout detected for node: " << node.node_id << ", triggering NODE_LEFT event";
                    node_impl_->handleNodeEvent(node.node_id, false);
                }
            }
        }

        // Clean up stale inbound queues
        cleanupStaleQueues();

        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
    }

    NEXUS_DEBUG("SHM-V3") << "Heartbeat loop stopped for " << node_id_;
}

void SharedMemoryTransportV3::cleanupStaleQueues() {
    if (!my_shm_) {
        return;
    }

    // 🔧 CRITICAL: Do NOT recycle inbound queues here!
    // Inbound queues are in OUR shared memory, used to receive messages.
    // Even if the sender node dies, we shouldn't touch these queues because:
    // 1. Our receive thread might still be accessing them
    // 2. There could be pending messages to process
    // 3. Race conditions can cause memory corruption
    //
    // Queue cleanup should only happen when:
    // - This node is shutting down (in destructor)
    // - We explicitly disconnect from a remote node
    //
    // For now, leave queues alone - they'll be cleaned up during node shutdown
    // TODO: Implement safer queue recycling with proper synchronization
}

std::string SharedMemoryTransportV3::generateShmName() {
    std::ostringstream oss;
    oss << "/librpc_node_" << getpid() << "_" << std::hex << std::setfill('0') << std::setw(8)
        << (std::hash<std::string>{}(node_id_)&0xFFFFFFFF);
    return oss.str();
}

// Add accessor PID to our own node header
void SharedMemoryTransportV3::addAccessor(pid_t pid) {
    if (!my_shm_) {
        return;
    }
    addAccessorToNode(&my_shm_->header, pid);
}

// Remove accessor PID from our own node header
void SharedMemoryTransportV3::removeAccessor(pid_t pid) {
    if (!my_shm_) {
        return;
    }
    removeAccessorFromNode(&my_shm_->header, pid);
}

// Add accessor PID to any node header (thread-safe)
void SharedMemoryTransportV3::addAccessorToNode(NodeHeader* header, pid_t pid) {
    if (!header || pid <= 0) {
        return;
    }

    // Try to find an empty slot or existing entry
    for (uint32_t i = 0; i < NodeHeader::MAX_ACCESSORS; ++i) {
        int32_t expected = 0;
        // Try to claim an empty slot
        if (header->accessor_pids[i].compare_exchange_strong(expected, pid, std::memory_order_release,
                                                             std::memory_order_relaxed)) {
            header->num_accessors.fetch_add(1, std::memory_order_release);
            NEXUS_DEBUG("SHM-V3") << "Added accessor PID " << pid << " to node (count=" << header->num_accessors.load()
                                  << ")";
            return;
        }
        // Already exists
        if (expected == pid) {
            return;
        }
    }
    NEXUS_WARN("SHM-V3") << "Failed to add accessor PID " << pid
                         << ": accessor array full (MAX_ACCESSORS=" << NodeHeader::MAX_ACCESSORS << ")";
}

// Remove accessor PID from any node header (thread-safe)
void SharedMemoryTransportV3::removeAccessorFromNode(NodeHeader* header, pid_t pid) {
    if (!header || pid <= 0) {
        return;
    }

    // Find and remove the PID
    for (uint32_t i = 0; i < NodeHeader::MAX_ACCESSORS; ++i) {
        int32_t expected = pid;
        if (header->accessor_pids[i].compare_exchange_strong(expected, 0, std::memory_order_release,
                                                             std::memory_order_relaxed)) {
            header->num_accessors.fetch_sub(1, std::memory_order_release);
            NEXUS_DEBUG("SHM-V3") << "Removed accessor PID " << pid
                                  << " from node (count=" << header->num_accessors.load() << ")";
            return;
        }
    }

    NEXUS_DEBUG("SHM-V3") << "Accessor PID " << pid << " not found in node header";
}

// Check if any accessor PIDs are still alive
bool SharedMemoryTransportV3::hasActiveAccessors(NodeHeader* header) {
    if (!header) {
        return false;
    }

    uint32_t count = header->num_accessors.load(std::memory_order_acquire);
    if (count == 0) {
        return false;
    }

    // Verify each accessor PID is actually alive
    bool has_active = false;
    for (uint32_t i = 0; i < NodeHeader::MAX_ACCESSORS; ++i) {
        pid_t pid = header->accessor_pids[i].load(std::memory_order_acquire);
        if (pid > 0) {
            // Check if process is alive
            if (kill(pid, 0) == 0) {
                has_active = true;
            } else {
                // Dead process, remove it
                int32_t expected = pid;
                if (header->accessor_pids[i].compare_exchange_strong(expected, 0, std::memory_order_release,
                                                                     std::memory_order_relaxed)) {
                    header->num_accessors.fetch_sub(1, std::memory_order_release);
                    NEXUS_DEBUG("SHM-V3") << "Removed dead accessor PID " << pid;
                }
            }
        }
    }
    return has_active;
}

}  // namespace rpc
}  // namespace Nexus
