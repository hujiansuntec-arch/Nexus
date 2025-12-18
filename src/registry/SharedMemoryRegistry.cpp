#include "nexus/registry/SharedMemoryRegistry.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>  // For flock()
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>  // For errno
#include <chrono>
#include <cstring>
#include <ctime>  // For time()
#include <iostream>
#include <set>  // For alive_pids deduplication
#include <thread>

#include "nexus/utils/Logger.h"

// QNX specific includes
#ifdef __QNXNTO__
#include <sys/neutrino.h>
#endif

namespace Nexus {
namespace rpc {

SharedMemoryRegistry::SharedMemoryRegistry()
    : initialized_(false), shm_ptr_(nullptr), shm_fd_(-1), registry_(nullptr) {}

SharedMemoryRegistry::~SharedMemoryRegistry() {
    if (initialized_ && registry_) {
        // 🔧 CRITICAL: 使用锁保护析构操作，防止并发访问
        int lock_result = pthread_mutex_lock(&registry_->header.global_lock);
        bool should_cleanup = false;

        if (lock_result == EOWNERDEAD) {
            // 上一个持有锁的进程崩溃，恢复锁状态
            NEXUS_LOG_WARN("Registry", "Recovered mutex from dead process in destructor");
            pthread_mutex_consistent(&registry_->header.global_lock);
        }

        if (lock_result == 0 || lock_result == EOWNERDEAD) {
            // 🔧 CRITICAL: 从 ref_pids 中移除当前进程PID
            pid_t my_pid = getpid();
            for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
                uint32_t expected = my_pid;
                if (registry_->header.ref_pids[i].compare_exchange_strong(expected, 0, std::memory_order_release)) {
                    break;
                }
            }

            // 🔧 递减引用计数
            uint32_t prev_count = registry_->header.ref_count.fetch_sub(1, std::memory_order_acq_rel);
            NEXUS_LOG_INFO("Registry", "Decremented ref_count: " + std::to_string(prev_count) + " -> " +
                                           std::to_string(prev_count - 1));

            // 🔧 修正：在锁内判断是否是最后一个，防止竞态条件
            // 再次检查当前 ref_count，确认是 0
            uint32_t current_count = registry_->header.ref_count.load(std::memory_order_acquire);
            should_cleanup = (current_count == 0);

            // 🔧 CRITICAL: 解锁但不立即销毁，等munmap后再销毁
            pthread_mutex_unlock(&registry_->header.global_lock);
        } else {
            NEXUS_LOG_ERROR("Registry", "Failed to acquire lock in destructor: " + std::string(strerror(lock_result)));
        }

        // 🔧 先 munmap 再 destroy mutex，防止访问已释放的内存
        if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
            munmap(shm_ptr_, sizeof(RegistryRegion));
            shm_ptr_ = nullptr;
        }
        if (shm_fd_ >= 0) {
            close(shm_fd_);
            shm_fd_ = -1;
        }

        registry_ = nullptr;

        if (should_cleanup) {
            // 最后一个进程，尝试清理
            cleanupOrphanedRegistry();
        }
    }
}

bool SharedMemoryRegistry::initialize() {
    if (initialized_) {
        return true;
    }

    // 🔧 尝试创建新的registry（使用O_EXCL确保原子性）
    shm_fd_ = shm_open(REGISTRY_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    bool creating = (shm_fd_ >= 0);

    if (!creating) {
        // Registry已存在，尝试打开
        shm_fd_ = shm_open(REGISTRY_SHM_NAME, O_RDWR, 0666);
        if (shm_fd_ < 0) {
            NEXUS_LOG_ERROR("Registry", "Failed to open registry: " + std::string(strerror(errno)));
            return false;
        }

        // 🔧 CRITICAL: 检查文件大小，防止 mmap 损坏的 registry
        struct stat st;
        if (fstat(shm_fd_, &st) == 0) {
            if (st.st_size == 0) {
                NEXUS_LOG_WARN("Registry", "Found empty registry file (size=0), previous creator may have failed");
                // 尝试重新设置大小
                if (ftruncate(shm_fd_, sizeof(RegistryRegion)) < 0) {
                    NEXUS_LOG_ERROR("Registry", "Failed to resize corrupted registry: " + std::string(strerror(errno)));
                    close(shm_fd_);
                    shm_fd_ = -1;
                    return false;
                }
                creating = true;  // 标记为需要初始化
                NEXUS_LOG_INFO("Registry", "Resized empty registry, will reinitialize");
            } else if (st.st_size != sizeof(RegistryRegion)) {
                NEXUS_LOG_ERROR("Registry", "Registry size incorrect: expected " +
                                                std::to_string(sizeof(RegistryRegion)) + ", got " +
                                                std::to_string(st.st_size));
                close(shm_fd_);
                shm_fd_ = -1;
                return false;
            }
        }
    } else {
        // 成功创建新registry，设置大小
        if (ftruncate(shm_fd_, sizeof(RegistryRegion)) < 0) {
            NEXUS_LOG_ERROR("Registry", "Failed to set size (errno=" + std::to_string(errno) +
                                            "): " + std::string(strerror(errno)));
            close(shm_fd_);
            shm_fd_ = -1;
            // 🔧 SAFE: 创建者在这里失败时可以删除，因为：
            // 1. magic 还未设置，其他进稌不会使用这个 registry
            // 2. 即使其他进程 shm_open 了，也会在等待 magic 时超时
            shm_unlink(REGISTRY_SHM_NAME);
            return false;
        }

        // 🔧 验证 ftruncate 是否成功设置了正确的大小
        struct stat st;
        if (fstat(shm_fd_, &st) == 0) {
            if (st.st_size != sizeof(RegistryRegion)) {
                NEXUS_LOG_ERROR("Registry", "Registry size mismatch: expected " +
                                                std::to_string(sizeof(RegistryRegion)) + ", got " +
                                                std::to_string(st.st_size));
                close(shm_fd_);
                shm_fd_ = -1;
                return false;
            }
        }
    }

    // Map memory
    shm_ptr_ = mmap(nullptr, sizeof(RegistryRegion), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        NEXUS_LOG_ERROR("Registry", "Failed to map memory: " + std::string(strerror(errno)));
        close(shm_fd_);
        shm_fd_ = -1;
        // 🔧 SAFE: 创建者在 mmap 失败时可以删除
        // 原因：其他进程即使 mmap 成功了，也会在等待 magic 时超时
        // 现在有了锁机制，所有访问都在锁保护下，更加安全
        if (creating) {
            shm_unlink(REGISTRY_SHM_NAME);
        }
        return false;
    }

    registry_ = static_cast<RegistryRegion*>(shm_ptr_);

    if (creating) {
        // Initialize header (先初始化其他字段，最后设置magic作为“就绪”标志)

        // 🔧 CRITICAL: 首先初始化进程间互斥锁
        pthread_mutexattr_t mutex_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);  // 跨进程共享
        pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST);     // 进程崩溃后锁可恢复
        pthread_mutex_init(&registry_->header.global_lock, &mutex_attr);
        pthread_mutexattr_destroy(&mutex_attr);

        // 🔧 使用 relaxed 初始化，因为 magic 还未设置，其他进程看不到
        registry_->header.version.store(VERSION, std::memory_order_relaxed);
        registry_->header.num_entries.store(0, std::memory_order_relaxed);
        registry_->header.capacity.store(MAX_REGISTRY_ENTRIES, std::memory_order_relaxed);

        // 🔧 初始化 ref_pids 数组（但不记录创建者 PID，等 magic 设置后再记录）
        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
            registry_->header.ref_pids[i].store(0, std::memory_order_relaxed);
        }

        // 🔧 ref_count 初始为 0，等 magic 设置后再递增
        registry_->header.ref_count.store(0, std::memory_order_relaxed);

        // Initialize all entries
        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
            registry_->entries[i].flags.store(0, std::memory_order_relaxed);
            // 🔧 Initialize atomic string arrays to zero
            for (int j = 0; j < 8; ++j) {
                registry_->entries[i].node_id_atomic[j].store(0, std::memory_order_relaxed);
                registry_->entries[i].shm_name_atomic[j].store(0, std::memory_order_relaxed);
            }
            registry_->entries[i].pid.store(0, std::memory_order_relaxed);
            registry_->entries[i].last_heartbeat.store(0, std::memory_order_relaxed);
        }

        // 🔧 CRITICAL: 在设置magic之前先记录ref_count=1，防止窗口期被误判为无人使用
        // 使用relaxed因为magic的release屏障会同步所有数据
        registry_->header.ref_pids[0].store(getpid(), std::memory_order_relaxed);
        registry_->header.ref_count.store(1, std::memory_order_relaxed);

        // 🔧 Memory barrier确保所有初始化完成后再设置magic
        std::atomic_thread_fence(std::memory_order_release);
        registry_->header.magic.store(MAGIC, std::memory_order_release);

        NEXUS_LOG_INFO("Registry", "Created new registry at " + std::string(REGISTRY_SHM_NAME));
    } else {
        // 🔧 等待并验证registry初始化完成
        bool valid = false;

        for (int retry = 0; retry < 100; ++retry) {
            // 🔧 直接读取 magic，acquire 语义已经足够，不需要 fence
            uint32_t magic = registry_->header.magic.load(std::memory_order_acquire);
            if (magic == MAGIC) {
                // 🔧 CRITICAL: 获取锁保护后续操作
                int lock_result = pthread_mutex_lock(&registry_->header.global_lock);

                if (lock_result == EOWNERDEAD) {
                    // 上一个持有锁的进程崩溃，恢复锁状态
                    NEXUS_LOG_WARN("Registry", "Recovered mutex from dead process during initialization");
                    pthread_mutex_consistent(&registry_->header.global_lock);
                    lock_result = 0;
                }

                if (lock_result == 0) {
                    // 再次检查 magic（双重检查）
                    if (registry_->header.magic.load(std::memory_order_acquire) == MAGIC) {
                        valid = true;

                        // 🔧 优化：先记录 PID 再递增 ref_count，防止崩溃时不一致
                        pid_t my_pid = getpid();
                        bool pid_recorded = false;

                        // 在 ref_pids 数组中找空位记录 PID
                        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
                            uint32_t expected = 0;
                            if (registry_->header.ref_pids[i].compare_exchange_strong(expected, my_pid,
                                                                                      std::memory_order_release)) {
                                pid_recorded = true;
                                break;
                            }
                        }

                        // 只有在 PID 记录成功后才递增 ref_count
                        if (pid_recorded) {
                            registry_->header.ref_count.fetch_add(1, std::memory_order_release);
                        } else {
                            NEXUS_LOG_ERROR("Registry", "Failed to record PID: ref_pids array full (" +
                                                            std::to_string(MAX_REGISTRY_ENTRIES) +
                                                            " processes already using registry)");
                            valid = false;
                            // 需要清理已分配的资源
                        }
                    }

                    pthread_mutex_unlock(&registry_->header.global_lock);

                    if (valid)
                        break;
                }
            }

            // Registry正在初始化中，等待一小段时间
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!valid) {
            NEXUS_LOG_ERROR("Registry", "Registry initialization timeout - magic number not set after 1000ms");

            // 🔧 CRITICAL: 检查是否是损坏的 registry（创建者崩溃或初始化失败）
            // 🔧 读取当前 magic 值来判断（atomic load 不会抛异常）
            uint32_t current_magic = registry_->header.magic.load(std::memory_order_acquire);

            // 清理当前映射
            registry_ = nullptr;
            if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
                munmap(shm_ptr_, sizeof(RegistryRegion));
                shm_ptr_ = nullptr;
            }
            close(shm_fd_);
            shm_fd_ = -1;

            // 🔧 如果 magic=0，说明 registry 被创建但未初始化完成
            if (current_magic == 0) {
                NEXUS_LOG_WARN("Registry", "Detected corrupted registry (magic=0), may need manual cleanup or restart");
            }

            return false;
        }

        NEXUS_LOG_INFO("Registry", "Opened existing registry with " +
                                       std::to_string(registry_->header.num_entries.load()) + " entries");
    }

    initialized_ = true;
    return true;
}

bool SharedMemoryRegistry::registerNode(const std::string& node_id, const std::string& shm_name) {
    if (!initialized_) {
        return false;
    }

    if (node_id.empty() || shm_name.empty()) {
        return false;
    }

    if (node_id.size() >= NODE_ID_SIZE || shm_name.size() >= SHM_NAME_SIZE) {
        NEXUS_LOG_ERROR("Registry", "Node ID or shm name too long");
        return false;
    }

    // 🔧 CRITICAL: 使用锁保护整个注册操作
    RegistryLock lock(&registry_->header.global_lock);
    if (!lock.isLocked()) {
        NEXUS_LOG_ERROR("Registry", "Failed to acquire lock for registerNode");
        return false;
    }

    // Check if already registered
    int existing_idx = findEntryIndex(node_id);
    if (existing_idx >= 0) {
        // Update existing entry
        RegistryEntry& entry = registry_->entries[existing_idx];

        // 🔧 Write using atomic operations for cross-process safety
        writeAtomicString(entry.shm_name_atomic, shm_name, SHM_NAME_SIZE);
        entry.pid.store(getpid(), std::memory_order_seq_cst);
        uint64_t update_ts = getCurrentTimeMs();
        entry.last_heartbeat.store(update_ts, std::memory_order_seq_cst);
        NEXUS_LOG_INFO("Registry", "[TIMESTAMP] registerNode (update): " + node_id +
                                       " updated_hb=" + std::to_string(update_ts) + "ms");

        // 🔧 Increment version to detect ABA problem
        entry.version.fetch_add(1, std::memory_order_release);

        // 🔧 Finally set flags to indicate entry is valid
        entry.flags.store(0x3, std::memory_order_seq_cst);  // valid | active

        NEXUS_LOG_INFO("Registry", "Updated node: " + node_id + " -> " + shm_name);
        return true;
    }

    // 🔧 CRITICAL: Atomically claim a free entry using CAS to prevent race conditions
    // Multiple processes may call registerNode() concurrently during startup
    // Without atomic allocation, they could get the same index and overwrite each other
    int idx = -1;
    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        uint32_t expected = 0;   // Free entry (flags == 0)
        uint32_t desired = 0x1;  // Claim it (valid bit set, but not active yet)

        // Try to atomically claim this entry
        if (registry_->entries[i].flags.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
            idx = static_cast<int>(i);
            break;
        }
    }

    if (idx < 0) {
        NEXUS_LOG_ERROR("Registry", "Registry full (max " + std::to_string(MAX_REGISTRY_ENTRIES) + " nodes)");
        return false;
    }

    // Register new node (we now have exclusive ownership of this entry)
    RegistryEntry& entry = registry_->entries[idx];

    // 🔧 CRITICAL: Write all fields using atomic operations for cross-process safety
    writeAtomicString(entry.node_id_atomic, node_id, NODE_ID_SIZE);
    writeAtomicString(entry.shm_name_atomic, shm_name, SHM_NAME_SIZE);
    entry.pid.store(getpid(), std::memory_order_seq_cst);
    uint64_t init_ts = getCurrentTimeMs();
    entry.last_heartbeat.store(init_ts, std::memory_order_seq_cst);
    NEXUS_LOG_INFO("Registry",
                   "[TIMESTAMP] registerNode (new): " + node_id + " initial_hb=" + std::to_string(init_ts) + "ms");

    // 🔧 Initialize version (use fetch_add for atomicity)
    entry.version.fetch_add(1, std::memory_order_release);

    // 🔧 CRITICAL: Set flags last to publish the entry atomically
    entry.flags.store(0x3, std::memory_order_seq_cst);  // valid | active

    // 🔧 Update num_entries with release so other processes see the new entry
    registry_->header.num_entries.fetch_add(1, std::memory_order_release);

    NEXUS_LOG_INFO("Registry", "Registered node: " + node_id + " -> " + shm_name + " (total: " +
                                   std::to_string(registry_->header.num_entries.load(std::memory_order_acquire)) + ")");

    return true;
}

bool SharedMemoryRegistry::unregisterNode(const std::string& node_id) {
    if (!initialized_) {
        return false;
    }

    // 🔧 CRITICAL: 使用锁保护注销操作
    RegistryLock lock(&registry_->header.global_lock);
    if (!lock.isLocked()) {
        NEXUS_LOG_ERROR("Registry", "Failed to acquire lock for unregisterNode");
        return false;
    }

    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }

    // Clear entry
    RegistryEntry& entry = registry_->entries[idx];

    // 🔧 CRITICAL: Decrement with release to ensure visibility
    registry_->header.num_entries.fetch_sub(1, std::memory_order_release);

    // 🔧 Clear flags with seq_cst to prevent other processes from seeing this entry
    entry.flags.store(0, std::memory_order_seq_cst);

    // 🔧 Increment version to invalidate any cached references (ABA protection)
    entry.version.fetch_add(1, std::memory_order_release);

    // 🔧 Now safe to clear other atomic fields (no one can see this entry anymore)
    // Use release to ensure visibility of the clear operation
    for (int j = 0; j < 8; ++j) {
        entry.node_id_atomic[j].store(0, std::memory_order_release);
        entry.shm_name_atomic[j].store(0, std::memory_order_release);
    }
    entry.pid.store(0, std::memory_order_release);
    entry.last_heartbeat.store(0, std::memory_order_release);

    NEXUS_LOG_INFO("Registry", "Unregistered node: " + node_id +
                                   " (remaining: " + std::to_string(registry_->header.num_entries.load()) + ")");

    return true;
}

bool SharedMemoryRegistry::updateHeartbeat(const std::string& node_id) {
    if (!initialized_) {
        return false;
    }

    // 🔧 CRITICAL: 心跳更新必须在锁保护下完成，防止节点在更新前被注销/重用
    RegistryLock lock(&registry_->header.global_lock, 1000);  // 1秒超时
    if (!lock.isLocked()) {
        NEXUS_LOG_ERROR("Registry", "Failed to acquire lock for updateHeartbeat");
        return false;
    }

    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }

    // 🔧 验证版本号防止ABA问题
    RegistryEntry& entry = registry_->entries[idx];
    uint32_t version_before = entry.version.load(std::memory_order_acquire);

    // 更新心跳时间戳
    uint64_t hb_ts = getCurrentTimeMs();
    entry.last_heartbeat.store(hb_ts, std::memory_order_release);

    // 再次验证版本号
    uint32_t version_after = entry.version.load(std::memory_order_acquire);
    if (version_before != version_after) {
        // Entry 在更新过程中被重用
        return false;
    }

    return true;
}

std::vector<NodeInfo> SharedMemoryRegistry::getAllNodes() const {
    std::vector<NodeInfo> nodes;

    if (!initialized_) {
        return nodes;
    }

    // 🔧 CRITICAL: 使用锁保护遍历操作
    RegistryLock lock(const_cast<pthread_mutex_t*>(&registry_->header.global_lock));
    if (!lock.isLocked()) {
        NEXUS_LOG_ERROR("Registry", "Failed to acquire lock for getAllNodes");
        return nodes;
    }

    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        const RegistryEntry& entry = registry_->entries[i];
        // 🔧 Use seq_cst for shared memory visibility
        uint32_t flags = entry.flags.load(std::memory_order_seq_cst);

        if ((flags & 0x1) == 0) {  // Not valid
            continue;
        }

        // 🔧 Read all fields atomically
        NodeInfo info;
        info.node_id = readAtomicString(entry.node_id_atomic, NODE_ID_SIZE);
        info.shm_name = readAtomicString(entry.shm_name_atomic, SHM_NAME_SIZE);
        info.pid = entry.pid.load(std::memory_order_seq_cst);
        info.last_heartbeat = entry.last_heartbeat.load(std::memory_order_seq_cst);
        info.active = (flags & 0x2) != 0;

        nodes.push_back(info);
    }

    return nodes;
}

bool SharedMemoryRegistry::findNode(const std::string& node_id, NodeInfo& info) const {
    if (!initialized_) {
        return false;
    }

    // 🔧 CRITICAL: 使用锁保护查找操作
    RegistryLock lock(const_cast<pthread_mutex_t*>(&registry_->header.global_lock));
    if (!lock.isLocked()) {
        NEXUS_LOG_ERROR("Registry", "Failed to acquire lock for findNode");
        return false;
    }

    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }

    const RegistryEntry& entry = registry_->entries[idx];

    // 🔧 Read all fields atomically
    info.node_id = readAtomicString(entry.node_id_atomic, NODE_ID_SIZE);
    info.shm_name = readAtomicString(entry.shm_name_atomic, SHM_NAME_SIZE);
    info.pid = entry.pid.load(std::memory_order_seq_cst);
    info.last_heartbeat = entry.last_heartbeat.load(std::memory_order_seq_cst);
    info.active = (entry.flags.load(std::memory_order_seq_cst) & 0x2) != 0;

    return true;
}

bool SharedMemoryRegistry::nodeExists(const std::string& node_id) const {
    if (!initialized_) {
        return false;
    }

    // 🔧 CRITICAL: 使用锁保护检查操作
    RegistryLock lock(const_cast<pthread_mutex_t*>(&registry_->header.global_lock));
    if (!lock.isLocked()) {
        return false;
    }

    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }

    // 🔧 使用acquire读取flags
    uint32_t flags = registry_->entries[idx].flags.load(std::memory_order_acquire);
    return (flags & 0x3) == 0x3;  // valid && active
}

int SharedMemoryRegistry::cleanupStaleNodes(uint64_t timeout_ms) {
    if (!initialized_) {
        return 0;
    }

    // 🔧 CRITICAL: 使用锁保护清理操作
    RegistryLock lock(&registry_->header.global_lock);
    if (!lock.isLocked()) {
        NEXUS_LOG_ERROR("Registry", "Failed to acquire lock for cleanupStaleNodes");
        return 0;
    }

    int cleaned = 0;
    uint64_t now = getCurrentTimeMs();

    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        RegistryEntry& entry = registry_->entries[i];
        // 🔧 Use seq_cst to see latest flags
        uint32_t flags = entry.flags.load(std::memory_order_seq_cst);

        if ((flags & 0x1) == 0) {  // Not valid
            continue;
        }

        // 🔧 Read pid and heartbeat atomically
        pid_t pid = entry.pid.load(std::memory_order_seq_cst);
        uint64_t last_hb = entry.last_heartbeat.load(std::memory_order_seq_cst);

        // 🔧 CRITICAL: Skip entries being initialized (last_hb == 0)
        // During concurrent registration with CAS, an entry may have valid bit set
        // but other fields (node_id, pid, last_hb) are still being written
        // Wait until initialization is complete before checking timeout
        if (last_hb == 0) {
            continue;  // Entry is being initialized, skip it
        }

        // 🔧 CRITICAL: 使用steady_clock后不会有时钟回退问题
        // 但仍需处理跨进程的时间戳（每个进程的steady_clock epoch不同）
        // 因此这里的超时检测仅作为辅助，主要依赖kill(pid,0)检测进程存活
        uint64_t time_since_hb = 0;
        if (now >= last_hb) {
            time_since_hb = now - last_hb;
        } else {
            // 跨进程时间戳不可比，视为新鲜心跳
            time_since_hb = 0;
        }

        bool timeout = time_since_hb > timeout_ms;
        bool process_dead = !isProcessAlive(pid);

        if (timeout || process_dead) {
            // 🔧 CRITICAL: 读取版本号，稍后验证以防止ABA问题
            uint32_t version_before = entry.version.load(std::memory_order_acquire);

            // Read node_id for logging
            std::string node_id_str = readAtomicString(entry.node_id_atomic, NODE_ID_SIZE);

            // 🔧 详细日志：显示心跳时间差，帮助诊断
            std::string reason;
            if (timeout) {
                reason = " (timeout: last_hb=" + std::to_string(last_hb) + "ms, now=" + std::to_string(now) +
                         "ms, diff=" + std::to_string(time_since_hb) + "ms > " + std::to_string(timeout_ms) + "ms)";
            } else {
                reason = " (process dead)";
            }

            NEXUS_LOG_INFO("Registry", "Cleaning stale node: " + node_id_str + reason);

            // 🔧 CRITICAL: 使用CAS验证版本号并清除flags，防止TOCTOU
            // 如果版本号已改变，说明entry被重用，跳过清理
            uint32_t expected_version = version_before;
            uint32_t new_version = version_before + 1;

            // 尝试递增版本号，如果成功说明没有被重用
            if (!entry.version.compare_exchange_strong(expected_version, new_version, std::memory_order_acq_rel)) {
                // Version changed, entry was reused, skip cleanup
                continue;
            }

            // 🔧 Clear flags to invalidate entry
            entry.flags.store(0, std::memory_order_seq_cst);

            // 🔧 Increment version to prevent ABA problem
            entry.version.fetch_add(1, std::memory_order_release);

            // 🔧 Clear other atomic fields (use release for visibility)
            entry.pid.store(0, std::memory_order_release);
            entry.last_heartbeat.store(0, std::memory_order_release);
            for (size_t j = 0; j < 8; ++j) {
                entry.node_id_atomic[j].store(0, std::memory_order_release);
                entry.shm_name_atomic[j].store(0, std::memory_order_release);
            }

            // 🔧 Decrement count (use release)
            registry_->header.num_entries.fetch_sub(1, std::memory_order_release);

            cleaned++;
        }
    }

    return cleaned;
}

int SharedMemoryRegistry::getActiveNodeCount() const {
    if (!initialized_) {
        return 0;
    }

    // 🔧 Use acquire to see latest count updates
    return registry_->header.num_entries.load(std::memory_order_acquire);
}

bool SharedMemoryRegistry::amICleanupMaster() const {
    if (!initialized_) {
        return false;
    }

    RegistryLock lock(&registry_->header.global_lock, 1000);
    if (!lock.isLocked()) {
        return false;  // Can't acquire lock, not safe to cleanup
    }

    pid_t my_pid = getpid();
    pid_t smallest_active_pid = INT32_MAX;

    // Find smallest PID among all active processes
    for (int i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        pid_t pid = registry_->header.ref_pids[i].load(std::memory_order_acquire);
        if (pid > 0) {
            // Verify process is still alive
            if (kill(pid, 0) == 0 || errno == EPERM) {
                if (pid < smallest_active_pid) {
                    smallest_active_pid = pid;
                }
            }
        }
    }

    // I'm the cleanup master if I have the smallest PID
    return (my_pid == smallest_active_pid);
}

bool SharedMemoryRegistry::cleanupOrphanedRegistry() {
    // Try to open registry
    int fd = shm_open(REGISTRY_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        return true;  // No registry to clean
    }

    // Map it
    void* ptr = mmap(nullptr, sizeof(RegistryRegion), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        // 🔧 mmap 失败不立即删除shm，可能其他进程正在使用
        // 让下次cleanup在获取锁后再判断是否删除
        NEXUS_LOG_ERROR("Registry", "Failed to mmap registry in cleanup: " + std::string(strerror(errno)));
        return false;
    }

    RegistryRegion* reg = static_cast<RegistryRegion*>(ptr);

    // 🔧 CRITICAL: 使用 pthread_mutex 而不是 flock，保持一致性
    // 尝试获取锁，如果获取失败说明有其他进程正在使用
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;  // 1秒超时

    int lock_result = pthread_mutex_timedlock(&reg->header.global_lock, &ts);

    if (lock_result == ETIMEDOUT) {
        NEXUS_LOG_INFO("Registry", "Registry is being used by another process, skipping cleanup");
        munmap(ptr, sizeof(RegistryRegion));
        close(fd);
        return true;
    }

    if (lock_result == EOWNERDEAD) {
        // 锁持有者崩溃，恢复锁并继续
        NEXUS_LOG_WARN("Registry", "Recovered mutex from dead process in cleanupOrphanedRegistry");
        pthread_mutex_consistent(&reg->header.global_lock);
    } else if (lock_result != 0) {
        NEXUS_LOG_ERROR("Registry", "Failed to acquire lock for cleanup: " + std::string(strerror(lock_result)));
        munmap(ptr, sizeof(RegistryRegion));
        close(fd);
        return true;
    }

    // 🔧 检查 magic number
    std::atomic_thread_fence(std::memory_order_acquire);
    if (reg->header.magic.load(std::memory_order_acquire) == MAGIC) {
        // Registry is valid, check ref_count and process liveness
        uint32_t ref_count = reg->header.ref_count.load(std::memory_order_acquire);

        // 🔧 CRITICAL: 检查 ref_pids 中的进程是否还存活
        // 如果进程崩溃，ref_count 可能不准确，需要实际检查进程
        // 使用 set 去重，避免同一进程在 ref_pids 和 entries 中被重复计数
        std::set<pid_t> alive_pids;

        // 检查 ref_pids
        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
            pid_t pid = reg->header.ref_pids[i].load(std::memory_order_acquire);
            if (pid > 0 && kill(pid, 0) == 0) {
                alive_pids.insert(pid);
            }
        }

        // 额外检查 entries 中的进程（自动去重）
        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
            uint32_t flags = reg->entries[i].flags.load(std::memory_order_acquire);
            if (flags & 0x1) {
                pid_t pid = reg->entries[i].pid.load(std::memory_order_acquire);
                if (pid > 0 && kill(pid, 0) == 0) {
                    alive_pids.insert(pid);
                }
            }
        }

        int alive_count = alive_pids.size();

        // 🔧 现在有了进程间互斥锁，可以安全地删除 registry
        // flock 确保没有其他进程正在访问

        if (alive_count == 0) {
            // 🔧 SAFE: 在锁保护下删除，不会有 Bus error
            pthread_mutex_unlock(&reg->header.global_lock);
            munmap(ptr, sizeof(RegistryRegion));
            close(fd);
            shm_unlink(REGISTRY_SHM_NAME);
            NEXUS_LOG_INFO("Registry",
                           "Cleaned up orphaned registry (alive_count=0, ref_count=" + std::to_string(ref_count) + ")");
        } else {
            NEXUS_LOG_INFO("Registry", "Registry has " + std::to_string(alive_count) + " alive processes (ref_count=" +
                                           std::to_string(ref_count) + "), keeping it");
            pthread_mutex_unlock(&reg->header.global_lock);
            munmap(ptr, sizeof(RegistryRegion));
            close(fd);
        }
    } else {
        // Magic number 无效，可能是正在初始化中或损坏
        // 🔧 此时已经持有锁（函数开头获取），直接检查
        uint32_t current_magic = reg->header.magic.load(std::memory_order_acquire);

        if (current_magic == 0) {
            // 确认损坏，删除并让下次初始化重建
            NEXUS_LOG_WARN("Registry", "Detected corrupted registry (magic=0), removing for rebuild");
            pthread_mutex_unlock(&reg->header.global_lock);
            munmap(ptr, sizeof(RegistryRegion));
            close(fd);
            shm_unlink(REGISTRY_SHM_NAME);
            return true;
        }

        // Magic 不是 0 也不是 MAGIC，可能正在初始化中
        pthread_mutex_unlock(&reg->header.global_lock);
        munmap(ptr, sizeof(RegistryRegion));
        close(fd);
        NEXUS_LOG_WARN("Registry",
                       "Found registry with invalid magic (" + std::to_string(current_magic) + "), skipping cleanup");
    }

    return true;
}

bool SharedMemoryRegistry::forceRemoveRegistry() {
    // 🔧 CRITICAL: 在删除前尝试获取锁，确保没有其他进程正在使用
    int fd = shm_open(REGISTRY_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        if (errno == ENOENT) {
            NEXUS_LOG_INFO("Registry", "Registry does not exist, nothing to remove");
            return true;
        }
        // 其他错误，尝试删除
        int result = shm_unlink(REGISTRY_SHM_NAME);
        return result == 0;
    }

    // 尝试mmap
    void* ptr = mmap(nullptr, sizeof(RegistryRegion), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        // mmap失败，直接删除
        int result = shm_unlink(REGISTRY_SHM_NAME);
        if (result == 0) {
            NEXUS_LOG_WARN("Registry", "Force removed registry: " + std::string(REGISTRY_SHM_NAME));
            return true;
        }
        return false;
    }

    RegistryRegion* reg = static_cast<RegistryRegion*>(ptr);

    // 尝试获取锁（短超时）
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;

    int lock_result = pthread_mutex_timedlock(&reg->header.global_lock, &ts);

    if (lock_result == EOWNERDEAD) {
        pthread_mutex_consistent(&reg->header.global_lock);
        lock_result = 0;
    }

    if (lock_result == 0) {
        // 成功获取锁，安全删除
        pthread_mutex_unlock(&reg->header.global_lock);
        munmap(ptr, sizeof(RegistryRegion));
        close(fd);
        int result = shm_unlink(REGISTRY_SHM_NAME);
        if (result == 0) {
            NEXUS_LOG_WARN("Registry", "Force removed registry: " + std::string(REGISTRY_SHM_NAME));
            return true;
        }
        return false;
    } else {
        // 无法获取锁，说明有其他进程正在使用
        munmap(ptr, sizeof(RegistryRegion));
        close(fd);
        NEXUS_LOG_ERROR("Registry", "Cannot force remove: registry is in use by another process");
        return false;
    }
}

// Private helper methods

int SharedMemoryRegistry::findEntryIndex(const std::string& node_id) const {
    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        const RegistryEntry& entry = registry_->entries[i];
        // 🔧 For shared memory across processes, use seq_cst for cache coherence
        uint32_t flags = entry.flags.load(std::memory_order_seq_cst);

        if ((flags & 0x1) == 0) {  // Not valid
            continue;
        }

        // 🔧 Read node_id atomically
        std::string entry_node_id = readAtomicString(entry.node_id_atomic, NODE_ID_SIZE);

        if (entry_node_id == node_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}


uint64_t SharedMemoryRegistry::getCurrentTimeMs() const {
    // 🔧 CRITICAL: 使用steady_clock避免NTP时钟回退影响
    // 虽然steady_clock在不同进程中epoch可能不同，但我们使用的是相对时间差
    // 只要同一进程内的心跳更新和检查使用同一时钟源即可
    // 注意：这意味着进程A无法检测进程B的心跳超时（只能通过kill检测存活）
    // 但这是可接受的，因为我们已经有kill(pid,0)作为主要的存活检测机制
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}

bool SharedMemoryRegistry::isProcessAlive(pid_t pid) const {
    if (pid <= 0) {
        return false;
    }
    return kill(pid, 0) == 0;
}

// 🔧 Atomic string helpers for cross-process safe string storage
void SharedMemoryRegistry::writeAtomicString(std::atomic<uint64_t>* atomic_array, const std::string& str,
                                             size_t max_bytes) {
    // Convert string to uint64_t chunks and write atomically
    const size_t num_chunks = max_bytes / sizeof(uint64_t);
    char buffer[max_bytes];
    std::memset(buffer, 0, max_bytes);
    std::strncpy(buffer, str.c_str(), max_bytes - 1);

    // Write each 8-byte chunk atomically with seq_cst for immediate visibility
    for (size_t i = 0; i < num_chunks; ++i) {
        uint64_t chunk;
        std::memcpy(&chunk, buffer + i * sizeof(uint64_t), sizeof(uint64_t));
        atomic_array[i].store(chunk, std::memory_order_seq_cst);
    }
}

std::string SharedMemoryRegistry::readAtomicString(const std::atomic<uint64_t>* atomic_array, size_t max_bytes) {
    // Read uint64_t chunks atomically and convert to string
    const size_t num_chunks = max_bytes / sizeof(uint64_t);
    char buffer[max_bytes];
    std::memset(buffer, 0, max_bytes);  // 🔧 初始化为全0，确保安全

    // Read each 8-byte chunk atomically with seq_cst
    for (size_t i = 0; i < num_chunks; ++i) {
        uint64_t chunk = atomic_array[i].load(std::memory_order_seq_cst);
        std::memcpy(buffer + i * sizeof(uint64_t), &chunk, sizeof(uint64_t));
    }

    buffer[max_bytes - 1] = '\0';  // Ensure null termination

    // 🔧 CRITICAL: 使用 strnlen 而不是依赖 null terminator
    // 防止读到损坏数据时越界访问
    size_t len = strnlen(buffer, max_bytes - 1);
    return std::string(buffer, len);
}

}  // namespace rpc
}  // namespace Nexus
