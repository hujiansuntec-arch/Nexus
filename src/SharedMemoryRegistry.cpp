#include "SharedMemoryRegistry.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>     // For errno
#include <chrono>
#include <signal.h>
#include <iostream>

// QNX specific includes
#ifdef __QNXNTO__
#include <sys/neutrino.h>
#endif

namespace librpc {

SharedMemoryRegistry::SharedMemoryRegistry()
    : initialized_(false)
    , shm_ptr_(nullptr)
    , shm_fd_(-1)
    , registry_(nullptr)
{
}

SharedMemoryRegistry::~SharedMemoryRegistry() {
    if (initialized_ && registry_) {
        // 🔧 修复：检查是否所有节点都已退出
        bool all_nodes_gone = true;
        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
            uint32_t flags = registry_->entries[i].flags.load();
            if (flags & 0x1) {  // 有效节点
                pid_t pid = registry_->entries[i].pid;
                if (isProcessAlive(pid)) {
                    all_nodes_gone = false;
                    break;
                }
            }
        }
        
        // 🔧 如果所有节点都退出，清理Registry
        if (all_nodes_gone) {
            std::cout << "[Registry] All nodes exited, cleaning up registry" << std::endl;
        }
    }
    
    if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
        munmap(shm_ptr_, sizeof(RegistryRegion));
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
    }
    
    // 🔧 修复：析构时检查并清理孤立的Registry
    if (initialized_) {
        // 尝试清理（只在所有进程都退出时才会真正unlink）
        cleanupOrphanedRegistry();
    }
}

bool SharedMemoryRegistry::initialize() {
    if (initialized_) {
        return true;
    }
    
    // Try to open existing registry
    shm_fd_ = shm_open(REGISTRY_SHM_NAME, O_RDWR, 0666);
    bool creating = (shm_fd_ < 0);
    
    if (creating) {
        // Create new registry
        shm_fd_ = shm_open(REGISTRY_SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (shm_fd_ < 0) {
            std::cerr << "[Registry] Failed to create registry: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Set size
        if (ftruncate(shm_fd_, sizeof(RegistryRegion)) < 0) {
            std::cerr << "[Registry] Failed to set size: " << strerror(errno) << std::endl;
            close(shm_fd_);
            shm_fd_ = -1;
            shm_unlink(REGISTRY_SHM_NAME);
            return false;
        }
    }
    
    // Map memory
    shm_ptr_ = mmap(nullptr, sizeof(RegistryRegion), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        std::cerr << "[Registry] Failed to map memory: " << strerror(errno) << std::endl;
        close(shm_fd_);
        shm_fd_ = -1;
        if (creating) {
            shm_unlink(REGISTRY_SHM_NAME);
        }
        return false;
    }
    
    registry_ = static_cast<RegistryRegion*>(shm_ptr_);
    
    if (creating) {
        // Initialize header
        registry_->header.magic.store(MAGIC);
        registry_->header.version.store(VERSION);
        registry_->header.num_entries.store(0);
        registry_->header.capacity.store(MAX_REGISTRY_ENTRIES);
        
        // Initialize all entries
        for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
            registry_->entries[i].flags.store(0);
            registry_->entries[i].node_id[0] = '\0';
            registry_->entries[i].shm_name[0] = '\0';
            registry_->entries[i].pid = 0;
            registry_->entries[i].last_heartbeat.store(0);
        }
        
        std::cout << "[Registry] Created new registry at " << REGISTRY_SHM_NAME << std::endl;
    } else {
        // Verify existing registry
        if (registry_->header.magic.load() != MAGIC) {
            std::cerr << "[Registry] Invalid magic number" << std::endl;
            munmap(shm_ptr_, sizeof(RegistryRegion));
            shm_ptr_ = nullptr;
            close(shm_fd_);
            shm_fd_ = -1;
            return false;
        }
        
        std::cout << "[Registry] Opened existing registry with " 
                  << registry_->header.num_entries.load() << " entries" << std::endl;
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
        std::cerr << "[Registry] Node ID or shm name too long" << std::endl;
        return false;
    }
    
    // Check if already registered
    int existing_idx = findEntryIndex(node_id);
    if (existing_idx >= 0) {
        // Update existing entry
        RegistryEntry& entry = registry_->entries[existing_idx];
        strncpy(entry.shm_name, shm_name.c_str(), SHM_NAME_SIZE - 1);
        entry.shm_name[SHM_NAME_SIZE - 1] = '\0';
        entry.pid = getpid();
        entry.last_heartbeat.store(getCurrentTimeMs());
        entry.flags.store(0x3);  // valid | active
        
        std::cout << "[Registry] Updated node: " << node_id << " -> " << shm_name << std::endl;
        return true;
    }
    
    // Find free entry
    int idx = findFreeEntryIndex();
    if (idx < 0) {
        std::cerr << "[Registry] Registry full (max " << MAX_REGISTRY_ENTRIES << " nodes)" << std::endl;
        return false;
    }
    
    // Register new node
    RegistryEntry& entry = registry_->entries[idx];
    strncpy(entry.node_id, node_id.c_str(), NODE_ID_SIZE - 1);
    entry.node_id[NODE_ID_SIZE - 1] = '\0';
    strncpy(entry.shm_name, shm_name.c_str(), SHM_NAME_SIZE - 1);
    entry.shm_name[SHM_NAME_SIZE - 1] = '\0';
    entry.pid = getpid();
    entry.last_heartbeat.store(getCurrentTimeMs());
    entry.flags.store(0x3);  // valid | active
    
    registry_->header.num_entries.fetch_add(1);
    
    std::cout << "[Registry] Registered node: " << node_id << " -> " << shm_name 
              << " (total: " << registry_->header.num_entries.load() << ")" << std::endl;
    
    return true;
}

bool SharedMemoryRegistry::unregisterNode(const std::string& node_id) {
    if (!initialized_) {
        return false;
    }
    
    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }
    
    // Clear entry
    RegistryEntry& entry = registry_->entries[idx];
    entry.flags.store(0);
    entry.node_id[0] = '\0';
    entry.shm_name[0] = '\0';
    entry.pid = 0;
    entry.last_heartbeat.store(0);
    
    registry_->header.num_entries.fetch_sub(1);
    
    std::cout << "[Registry] Unregistered node: " << node_id 
              << " (remaining: " << registry_->header.num_entries.load() << ")" << std::endl;
    
    return true;
}

bool SharedMemoryRegistry::updateHeartbeat(const std::string& node_id) {
    if (!initialized_) {
        return false;
    }
    
    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }
    
    registry_->entries[idx].last_heartbeat.store(getCurrentTimeMs());
    return true;
}

std::vector<NodeInfo> SharedMemoryRegistry::getAllNodes() const {
    std::vector<NodeInfo> nodes;
    
    if (!initialized_) {
        return nodes;
    }
    
    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        const RegistryEntry& entry = registry_->entries[i];
        uint32_t flags = entry.flags.load();
        
        if ((flags & 0x1) == 0) {  // Not valid
            continue;
        }
        
        NodeInfo info;
        info.node_id = entry.node_id;
        info.shm_name = entry.shm_name;
        info.pid = entry.pid;
        info.last_heartbeat = entry.last_heartbeat.load();
        info.active = (flags & 0x2) != 0;
        
        nodes.push_back(info);
    }
    
    return nodes;
}

bool SharedMemoryRegistry::findNode(const std::string& node_id, NodeInfo& info) const {
    if (!initialized_) {
        return false;
    }
    
    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }
    
    const RegistryEntry& entry = registry_->entries[idx];
    info.node_id = entry.node_id;
    info.shm_name = entry.shm_name;
    info.pid = entry.pid;
    info.last_heartbeat = entry.last_heartbeat.load();
    info.active = (entry.flags.load() & 0x2) != 0;
    
    return true;
}

bool SharedMemoryRegistry::nodeExists(const std::string& node_id) const {
    if (!initialized_) {
        return false;
    }
    
    int idx = findEntryIndex(node_id);
    if (idx < 0) {
        return false;
    }
    
    uint32_t flags = registry_->entries[idx].flags.load();
    return (flags & 0x3) == 0x3;  // valid && active
}

int SharedMemoryRegistry::cleanupStaleNodes(uint64_t timeout_ms) {
    if (!initialized_) {
        return 0;
    }
    
    int cleaned = 0;
    uint64_t now = getCurrentTimeMs();
    
    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        RegistryEntry& entry = registry_->entries[i];
        uint32_t flags = entry.flags.load();
        
        if ((flags & 0x1) == 0) {  // Not valid
            continue;
        }
        
        uint64_t last_hb = entry.last_heartbeat.load();
        bool timeout = (now - last_hb) > timeout_ms;
        bool process_dead = !isProcessAlive(entry.pid);
        
        if (timeout || process_dead) {
            std::cout << "[Registry] Cleaning stale node: " << entry.node_id 
                      << (timeout ? " (timeout)" : " (process dead)") << std::endl;
            
            entry.flags.store(0);
            entry.node_id[0] = '\0';
            entry.shm_name[0] = '\0';
            entry.pid = 0;
            entry.last_heartbeat.store(0);
            
            registry_->header.num_entries.fetch_sub(1);
            cleaned++;
        }
    }
    
    return cleaned;
}

int SharedMemoryRegistry::getActiveNodeCount() const {
    if (!initialized_) {
        return 0;
    }
    
    return registry_->header.num_entries.load();
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
        shm_unlink(REGISTRY_SHM_NAME);
        return true;
    }
    
    RegistryRegion* reg = static_cast<RegistryRegion*>(ptr);
    
    // Check if any process is alive
    bool has_alive = false;
    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        uint32_t flags = reg->entries[i].flags.load();
        if ((flags & 0x1) && kill(reg->entries[i].pid, 0) == 0) {
            has_alive = true;
            break;
        }
    }
    
    munmap(ptr, sizeof(RegistryRegion));
    close(fd);
    
    if (!has_alive) {
        shm_unlink(REGISTRY_SHM_NAME);
        std::cout << "[Registry] Cleaned up orphaned registry" << std::endl;
    }
    
    return true;
}

// Private helper methods

int SharedMemoryRegistry::findEntryIndex(const std::string& node_id) const {
    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        const RegistryEntry& entry = registry_->entries[i];
        if ((entry.flags.load() & 0x1) && strcmp(entry.node_id, node_id.c_str()) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int SharedMemoryRegistry::findFreeEntryIndex() const {
    for (size_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        if ((registry_->entries[i].flags.load() & 0x1) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint64_t SharedMemoryRegistry::getCurrentTimeMs() const {
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

} // namespace librpc
