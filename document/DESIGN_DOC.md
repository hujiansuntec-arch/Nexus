# LibRPC 详细设计文档

**版本**: 3.0  
**日期**: 2025-11-28  
**状态**: 已实施

---

## 目录

1. [概述](#1-概述)
2. [系统架构](#2-系统架构)
3. [核心组件设计](#3-核心组件设计)
4. [Node生命周期管理](#4-node生命周期管理)
5. [服务发现机制](#5-服务发现机制)
6. [通知机制设计](#6-通知机制设计)
7. [性能优化设计](#7-性能优化设计)
8. [资源管理设计](#8-资源管理设计)
9. [错误处理设计](#9-错误处理设计)
10. [性能指标](#10-性能指标)
11. [设计决策与权衡](#11-设计决策与权衡)

---

## 1. 概述

### 1.1 设计目标

LibRPC是一个专为QNX车载系统设计的高性能IPC框架，核心设计目标：

- **低延迟**: P50 < 10μs（跨进程通信）
- **高吞吐**: > 50,000 msg/s @ 256字节
- **低CPU占用**: < 6%（稳定负载）
- **内存效率**: 默认40MB/节点（64队列配置），可配置
- **高可靠性**: 异常退出2-5秒内自动恢复

### 1.2 关键创新

1. **双通知机制**：Condition Variable + Semaphore双重选择
2. **批量通知优化**：pending_msgs 0→1才触发通知，减少60%唤醒
3. **缓存活跃队列**：避免每次遍历MAX_INBOUND_QUEUES
4. **自适应超时**：空闲50ms、繁忙5ms动态切换
5. **移除FIFO方案**：简化架构，仅保留两种高效机制

---

## 2. 系统架构

### 2.1 整体架构图

```
┌──────────────────────────────────────────────────────────────┐
│                       Application Layer                       │
│   Node::publish() | subscribe() | sendLargeData()             │
└────────────────────────────┬─────────────────────────────────┘
                             │
┌────────────────────────────┴─────────────────────────────────┐
│                      Node Interface                           │
│  • Topic Routing        • Callback Management                 │
│  • Node Discovery       • Transport Selection                 │
└────────────────────────────┬─────────────────────────────────┘
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
┌───────▼──────┐  ┌──────────▼─────────┐  ┌──────▼─────────┐
│ InProcess    │  │ SharedMemory V3    │  │ LargeDataChannel│
│ Transport    │  │ Transport          │  │ (Zero-copy)     │
│              │  │                    │  │                 │
│ 直接调用      │  │ 无锁SPSC队列      │  │ 64MB环形缓冲    │
│ <1μs延迟     │  │ ~10μs延迟         │  │ ~135 MB/s       │
└──────────────┘  └──────────────────────┘  └─────────────────┘
                             │
                    ┌────────┴────────┐
                    │                 │
            ┌───────▼──────┐  ┌───────▼─────┐
            │   Registry   │  │  Node SHM   │
            │  (Discovery) │  │  (RX Queue) │
            └──────────────┘  └─────────────┘
```

### 2.2 进程间通信流程

```
Node A (Process 1)                     Node B (Process 2)
     │                                      │
     │ 1. publish("sensor", "temp", data)   │
     │                                      │
     ├─→ 2. 路由到SharedMemory Transport   │
     │                                      │
     │   3. 查找NodeB的InboundQueue        │
     │      (my_queue指针)                 │
     │                                      │
     │   4. tryWrite() 写入SPSC队列        │
     │      [data] → RingBuffer            │
     │                                      │
     │   5. pending_msgs++ (atomic)        │
     │      if (prev == 0) → 触发通知      │
     │                                      │
     │   ┌─────────────────────────┐       │
     │   │ Condition Variable:     │       │
     │   │   pthread_cond_signal() │       │
     │   │ OR Semaphore:           │       │
     │   │   sem_post()            │       │
     │   └─────────────────────────┘       │
     │                                      │
     │                                      ├─→ 6. receiveLoop等待中
     │                                      │
     │                                      │   pthread_cond_timedwait()
     │                                      │   OR sem_timedwait()
     │                                      │
     │                                      │   7. 被唤醒
     │                                      │
     │                                      │   8. 批量处理消息
     │                                      │      while (tryRead(...))
     │                                      │        callback(data)
     │                                      │
     │                                      │   9. pending_msgs -= processed
     │                                      │
     │                                      ◄── 10. 回调用户代码
```

---

## 3. 核心组件设计

### 3.1 SharedMemoryTransportV3

**核心数据结构**：

```cpp
struct NodeSharedMemory {
    struct Header {
        std::atomic<uint32_t> num_queues;      // 当前队列数
        std::atomic<uint32_t> max_queues;      // 最大队列数
        std::atomic<bool> ready;               // 初始化完成标志
        std::atomic<int32_t> pid;              // 拥有者PID
        std::atomic<int32_t> ref_count;        // 引用计数
        std::atomic<uint64_t> last_heartbeat;  // 心跳时间戳
    } header;
    
    InboundQueue queues[MAX_INBOUND_QUEUES];  // 入站队列数组
};

struct InboundQueue {
    // 队列标识
    char sender_id[64];                        // 发送者ID
    std::atomic<uint32_t> flags;               // 状态标志（bit0=valid, bit1=active）
    
    // 无锁环形队列
    LockFreeRingBuffer<QUEUE_CAPACITY> queue;  // SPSC队列
    
    // 通知机制（二选一）
    union {
        struct {  // Condition Variable模式
            pthread_mutex_t notify_mutex;
            pthread_cond_t notify_cond;
        };
        sem_t notify_sem;  // Semaphore模式
    };
    
    // 流控与统计
    std::atomic<uint32_t> pending_msgs;        // 待处理消息数
    std::atomic<uint32_t> drop_count;          // 丢弃计数
    std::atomic<uint32_t> congestion_level;    // 拥塞级别
    
    char padding[...];  // 对齐到缓存行
};
```

**关键方法**：

```cpp
class SharedMemoryTransportV3 {
public:
    // 发送消息（Fast Path）
    bool send(const std::string& dest_node_id, 
              const uint8_t* data, size_t size) {
        // 1. 快速查找连接（无锁）
        auto* conn = findConnection(dest_node_id);
        if (!conn) return false;  // Slow path
        
        // 2. 写入SPSC队列（无锁）
        if (!conn->my_queue->queue.tryWrite(node_id_, data, size)) {
            return false;  // 队列满
        }
        
        // 3. 批量通知优化
        uint32_t prev = conn->my_queue->pending_msgs.fetch_add(1, 
                            std::memory_order_release);
        if (prev == 0) {
            // 只在0→1时触发通知
            if (notify_mechanism_ == SEMAPHORE) {
                sem_post(&conn->my_queue->notify_sem);
            } else {
                pthread_cond_signal(&conn->my_queue->notify_cond);
            }
        }
        return true;
    }
    
    // 接收循环（Condition Variable版）
    void receiveLoop_CV() {
        std::vector<InboundQueue*> active_queues;  // 缓存活跃队列
        int consecutive_empty_loops = 0;
        
        while (receiving_) {
            // 1. 定期刷新活跃队列列表
            if (shouldRefreshQueues()) {
                refreshActiveQueues(active_queues);
            }
            
            // 2. 批量处理所有队列的消息
            bool has_messages = false;
            for (auto* q : active_queues) {
                // 验证队列仍有效
                if ((q->flags & 0x3) != 0x3) continue;
                
                // 批量处理该队列
                int processed = processQueueMessages(q);
                if (processed > 0) {
                    has_messages = true;
                    // 减少pending计数
                    q->pending_msgs.fetch_sub(processed);
                }
            }
            
            // 3. 无消息时等待（自适应超时）
            if (!has_messages) {
                int timeout_ms = (consecutive_empty_loops > 100) ? 50 : 5;
                waitForNotification(active_queues[0], timeout_ms);
                consecutive_empty_loops++;
            } else {
                consecutive_empty_loops = 0;
            }
        }
    }
};
```

### 3.2 LockFreeRingBuffer

**无锁SPSC变长队列设计**：

```cpp
template <size_t BUFFER_SIZE>
class LockFreeRingBuffer {
public:
    // 变长消息帧头 (8字节)
    struct FrameHeader {
        uint32_t length; // 总长度（含Header），若为0则表示Padding
        uint32_t magic;  // 校验魔数 (0xCAFEBABE)
    };

private:
    // 连续字节缓冲区，支持变长消息
    alignas(64) uint8_t buffer_[BUFFER_SIZE]; 
    
    // 缓存行对齐的原子指针，避免伪共享
    alignas(64) std::atomic<uint64_t> head_;  // 写位置（字节偏移）
    alignas(64) std::atomic<uint64_t> tail_;  // 读位置（字节偏移）
    
public:
    // 写入（单生产者）
    bool tryWrite(const uint8_t* data, size_t size) {
        // 1. 计算所需空间（Header + Data + 8字节对齐）
        size_t needed = (sizeof(FrameHeader) + size + 7) & ~7;
        
        uint64_t head = head_.load(std::memory_order_acquire);
        uint64_t tail = tail_.load(std::memory_order_acquire);
        
        // 2. 检查空间并处理环形回绕
        // 如果尾部空间不足，填充Padding并回绕到头部
        
        // 3. 写入Header和Data
        // ...
        
        // 4. 更新head指针（release语义）
        head_.store(new_head, std::memory_order_release);
        return true;
    }
    
    // 读取（单消费者）
    bool tryRead(uint8_t* buffer, size_t max_size, size_t& out_size) {
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        uint64_t head = head_.load(std::memory_order_acquire);
        
        // 1. 检查是否为空
        if (tail == head) return false;
        
        // 2. 读取Header
        // 处理Padding（如果遇到Padding Header，跳过并回绕）
        
        // 3. 读取数据
        // ...
        
        // 4. 更新tail指针（release语义）
        tail_.store(new_tail, std::memory_order_release);
        return true;
    }
        return true;
    }
};
```

**关键特性**：
- ✅ 单生产者单消费者（SPSC）
- ✅ 无锁实现（仅atomic操作）
- ✅ 内存顺序优化（relaxed + acquire/release）
- ✅ 缓存友好（slot连续分配）

---

## 4. Node生命周期管理

### 4.1 Node注册机制

Node注册是通过**SharedMemoryRegistry**实现的全局节点注册表，所有跨进程节点共享同一个Registry。

#### 4.1.1 Registry数据结构

```cpp
// Registry共享内存布局 (/librpc_registry)
struct RegistrySharedMemory {
    struct Header {
        std::atomic<uint32_t> magic;         // 魔数 0x4C525247 "LRRG"
        std::atomic<uint32_t> version;       // 版本号
        std::atomic<uint32_t> num_entries;   // 当前注册节点数
        uint32_t capacity;                   // 最大容量（256）
        std::atomic<uint64_t> sequence;      // 序列号（检测变化）
    } header;
    
    RegistryEntry entries[256];  // 节点条目数组
};

struct RegistryEntry {
    char node_id[64];                        // 节点ID
    char shm_name[64];                       // 节点共享内存名称
    std::atomic<int32_t> pid;                // 进程PID
    std::atomic<uint64_t> last_heartbeat;    // 最后心跳时间戳
    std::atomic<uint32_t> flags;             // 状态标志
    // bit0=valid（条目有效）
    // bit1=active（节点活跃）
};
```

#### 4.1.2 Registry初始化（原子创建）

```cpp
bool SharedMemoryRegistry::initialize() {
    // 1. 尝试以O_EXCL方式创建（仅创建者成功）
    int fd = shm_open("/librpc_registry", O_CREAT | O_EXCL | O_RDWR, 0666);
    
    if (fd >= 0) {
        // 我是创建者
        ftruncate(fd, sizeof(RegistrySharedMemory));
        void* ptr = mmap(nullptr, sizeof(RegistrySharedMemory), 
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        
        RegistrySharedMemory* registry = static_cast<RegistrySharedMemory*>(ptr);
        
        // 2. 初始化所有字段
        memset(registry, 0, sizeof(RegistrySharedMemory));
        registry->header.version.store(1, std::memory_order_relaxed);
        registry->header.num_entries.store(0, std::memory_order_relaxed);
        registry->header.capacity = 256;
        
        // 3. 最后写入魔数（release语义，确保前面的初始化可见）
        registry->header.magic.store(0x4C525247, std::memory_order_release);
        
        NEXUS_DEBUG("Registry") << "Created new registry";
        return true;
    }
    
    // 已存在，打开并验证
    if (errno == EEXIST) {
        fd = shm_open("/librpc_registry", O_RDWR, 0666);
        if (fd < 0) return false;
        
        void* ptr = mmap(nullptr, sizeof(RegistrySharedMemory), 
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        
        RegistrySharedMemory* registry = static_cast<RegistrySharedMemory*>(ptr);
        
        // 4. 验证魔数（acquire语义，确保看到完整的初始化）
        int retry = 0;
        while (retry < 10) {
            uint32_t magic = registry->header.magic.load(std::memory_order_acquire);
            if (magic == 0x4C525247) {
                NEXUS_DEBUG("Registry") << "Opened existing registry";
                return true;
            }
            // 创建者可能还在初始化，等待10ms
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            retry++;
        }
        
        NEXUS_ERROR("Registry") << "Invalid magic number after retry";
        return false;
    }
    
    return false;
}
```

**关键点**：
1. ✅ **O_EXCL原子创建**：确保只有一个进程创建Registry
2. ✅ **Release-Acquire语义**：创建者最后写魔数（release），读取者先验证魔数（acquire）
3. ✅ **重试机制**：读取者最多等待100ms，避免初始化窗口期的"Invalid magic"错误
4. ✅ **双重检查**：每次访问前验证魔数，防止使用未初始化的Registry

#### 4.1.3 Node注册流程

```cpp
bool SharedMemoryRegistry::registerNode(const std::string& node_id, 
                                       const std::string& shm_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 1. 查找空闲slot
    for (int i = 0; i < registry_->header.capacity; ++i) {
        RegistryEntry& entry = registry_->entries[i];
        uint32_t flags = entry.flags.load(std::memory_order_relaxed);
        
        if (flags & 0x1) continue;  // 已占用
        
        // 2. 填充节点信息
        strncpy(entry.node_id, node_id.c_str(), 63);
        strncpy(entry.shm_name, shm_name.c_str(), 63);
        entry.pid.store(getpid(), std::memory_order_relaxed);
        entry.last_heartbeat.store(getCurrentTimestamp(), 
                                  std::memory_order_relaxed);
        
        // 3. 设置标志位（release语义，确保信息可见）
        entry.flags.store(0x3, std::memory_order_release);  // valid | active
        
        // 4. 增加计数
        registry_->header.num_entries.fetch_add(1, std::memory_order_release);
        
        NEXUS_DEBUG("Registry") << "Registered node: " << node_id 
                                << " -> " << shm_name;
        return true;
    }
    
    NEXUS_ERROR("Registry") << "Registry full (capacity: " 
                            << registry_->header.capacity << ")";
    return false;
}
```

**注册信息**：
- `node_id`: 节点唯一标识（如 "node_18f2a3b4c5d6"）
- `shm_name`: 节点共享内存名称（如 "/librpc_node_12345_abcd1234"）
- `pid`: 进程PID（用于检测进程退出）
- `last_heartbeat`: 心跳时间戳（用于超时检测）

### 4.2 Node连接机制

Node之间的连接是**懒惰建立**的，即在**第一次发送消息时**才建立连接。

#### 4.2.1 连接建立流程

```cpp
bool SharedMemoryTransportV3::connectToNode(const std::string& target_node_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    
    // 1. 检查是否已连接
    auto it = remote_connections_.find(target_node_id);
    if (it != remote_connections_.end() && it->second.connected) {
        return true;  // 已连接，直接返回
    }
    
    // 2. 从Registry查询目标节点信息
    NodeInfo target_info;
    if (!registry_.findNode(target_node_id, target_info)) {
        NEXUS_ERROR("SHM-V3") << "Node not found: " << target_node_id;
        return false;
    }
    
    // 3. 打开目标节点的共享内存
    int shm_fd = shm_open(target_info.shm_name.c_str(), O_RDWR, 0666);
    if (shm_fd < 0) {
        NEXUS_ERROR("SHM-V3") << "Failed to open shm: " 
                              << target_info.shm_name;
        return false;
    }
    
    // 4. 映射目标节点的共享内存
    void* shm_ptr = mmap(nullptr, sizeof(NodeSharedMemory), 
                        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        close(shm_fd);
        return false;
    }
    
    NodeSharedMemory* remote_shm = static_cast<NodeSharedMemory*>(shm_ptr);
    
    // 5. 验证魔数和ready标志
    if (remote_shm->header.magic.load() != MAGIC) {
        munmap(shm_ptr, sizeof(NodeSharedMemory));
        close(shm_fd);
        return false;
    }
    
    if (!remote_shm->header.ready.load(std::memory_order_acquire)) {
        // 目标节点还未完全初始化
        munmap(shm_ptr, sizeof(NodeSharedMemory));
        close(shm_fd);
        return false;
    }
    
    // 6. 在目标节点的共享内存中分配入站队列
    InboundQueue* my_queue = findOrCreateQueue(remote_shm, node_id_);
    if (!my_queue) {
        munmap(shm_ptr, sizeof(NodeSharedMemory));
        close(shm_fd);
        return false;
    }
    
    // 7. 保存连接信息
    RemoteConnection conn;
    conn.node_id = target_node_id;
    conn.shm_name = target_info.shm_name;
    conn.shm_fd = shm_fd;
    conn.shm_ptr = shm_ptr;
    conn.my_queue = my_queue;  // 我在目标节点的入站队列
    conn.connected = true;
    
    remote_connections_[target_node_id] = conn;
    
    NEXUS_DEBUG("SHM-V3") << "Connected to node: " << target_node_id;
    return true;
}
```

#### 4.2.2 入站队列分配

```cpp
InboundQueue* SharedMemoryTransportV3::findOrCreateQueue(
    NodeSharedMemory* remote_shm, const std::string& sender_id) {
    
    // 1. 查找现有队列
    uint32_t num_queues = remote_shm->header.num_queues.load();
    for (uint32_t i = 0; i < num_queues && i < MAX_INBOUND_QUEUES; ++i) {
        InboundQueue& q = remote_shm->queues[i];
        if ((q.flags.load() & 0x1) && 
            strcmp(q.sender_id, sender_id.c_str()) == 0) {
            return &q;  // 找到现有队列
        }
    }
    
    // 2. 分配新队列
    for (size_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        InboundQueue& q = remote_shm->queues[i];
        
        // 原子地尝试占用空闲slot
        uint32_t expected = 0;
        if (q.flags.compare_exchange_strong(expected, 0x3)) {
            // 成功占用，初始化队列
            strncpy(q.sender_id, sender_id.c_str(), 63);
            q.pending_msgs.store(0);
            q.drop_count.store(0);
            q.congestion_level.store(0);
            
            // 根据通知机制初始化同步原语
            if (notify_mechanism_ == SEMAPHORE) {
                sem_init(&q.notify_sem, 1, 0);  // 进程间共享
            } else {
                pthread_mutexattr_t mattr;
                pthread_mutexattr_init(&mattr);
                pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
                pthread_mutex_init(&q.notify_mutex, &mattr);
                pthread_mutexattr_destroy(&mattr);
                
                pthread_condattr_t cattr;
                pthread_condattr_init(&cattr);
                pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
                pthread_cond_init(&q.notify_cond, &cattr);
                pthread_condattr_destroy(&cattr);
            }
            
            remote_shm->header.num_queues.fetch_add(1);
            
            NEXUS_DEBUG("SHM-V3") << "Created queue for sender: " << sender_id;
            return &q;
        }
    }
    
    return nullptr;  // 队列已满
}
```

#### 4.2.3 连接缓存与复用

```cpp
bool SharedMemoryTransportV3::send(const std::string& dest_node_id, 
                                  const uint8_t* data, size_t size) {
    // Fast Path: 检查连接缓存
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = remote_connections_.find(dest_node_id);
        if (it != remote_connections_.end() && it->second.connected) {
            // 连接已建立，直接发送
            InboundQueue* queue = it->second.my_queue;
            return queue->queue.tryWrite(node_id_.c_str(), data, size);
        }
    }
    
    // Slow Path: 建立连接
    if (!connectToNode(dest_node_id)) {
        return false;
    }
    
    // 重试发送
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = remote_connections_.find(dest_node_id);
        if (it != remote_connections_.end()) {
            return it->second.my_queue->queue.tryWrite(
                node_id_.c_str(), data, size);
        }
    }
    
    return false;
}
```

**连接关系图**：

```
Node A (Process 1)              Node B (Process 2)
┌─────────────────┐            ┌─────────────────┐
│ NodeSharedMemory│            │ NodeSharedMemory│
│                 │            │                 │
│ Header          │            │ Header          │
│ queues[0-63]    │            │ queues[0-63]    │
│   [0]: ← B     │◄───────┐   │   [0]: ← A     │
│   [1]: ← C     │        │   │   [1]: free    │
│   ...          │        │   │   ...          │
└─────────────────┘        │   └─────────────────┘
                           │            ▲
RemoteConnection to B      │            │
├─ shm_fd: 5              │            │
├─ shm_ptr: 0x7f...       │            │
└─ my_queue: &queues[0]───┘            │
                                       │
RemoteConnection to A                  │
├─ shm_fd: 4                           │
├─ shm_ptr: 0x7f...                    │
└─ my_queue: &queues[0]────────────────┘
```

### 4.3 Node发现机制

Node发现**不通过定期扫描Registry**，而是通过以下机制：

#### 4.3.1 启动时服务查询

```cpp
void NodeImpl::initialize() {
    // ... 初始化共享内存传输 ...
    
    // 查询现有服务（触发连接建立）
    queryRemoteServices();
    
    // 广播NODE_JOIN事件
    broadcastNodeEvent(true);
}

void NodeImpl::queryRemoteServices() {
    if (!shm_transport_v3_ || !shm_transport_v3_->isInitialized()) {
        return;
    }
    
    // 发送空的SERVICE_REGISTER消息作为查询
    // payload_len = 0 表示"查询"而非"注册"
    std::vector<uint8_t> query_packet = MessageBuilder::build(
        node_id_, "", "", nullptr, 0, 0, MessageType::SERVICE_REGISTER);
    
    // 广播查询消息（触发连接建立）
    shm_transport_v3_->broadcast(query_packet.data(), query_packet.size());
}
```

**广播流程**：

```cpp
int SharedMemoryTransportV3::broadcast(const uint8_t* data, size_t size) {
    // 1. 从Registry获取所有节点
    std::vector<NodeInfo> nodes = registry_.getAllNodes();
    
    int sent_count = 0;
    for (const auto& node : nodes) {
        if (node.node_id == node_id_) continue;  // 跳过自己
        
        // 2. 发送消息（自动建立连接）
        if (node.active && send(node.node_id, data, size)) {
            sent_count++;
        }
    }
    
    return sent_count;
}
```

#### 4.3.2 接收消息时被动发现

```cpp
// receiveLoop接收到消息后，NodeImpl处理
void NodeImpl::handleServiceMessage(const std::string& from_node,
                                   const std::string& group,
                                   const std::string& topic,
                                   const uint8_t* payload,
                                   size_t payload_len) {
    if (payload_len == 0) {
        // 收到服务查询，回复自己的服务列表
        auto services = GlobalRegistry::instance().getServices(group);
        for (const auto& svc : services) {
            broadcastServiceUpdate(svc, true);
        }
    } else {
        // 收到服务注册/注销
        ServiceDescriptor svc;
        deserializeService(payload, payload_len, svc);
        
        // 更新服务注册表
        GlobalRegistry::instance().registerService(group, svc);
        
        // 通知订阅者
        notifyServiceUpdate(svc);
    }
}
```

#### 4.3.3 心跳超时检测

```cpp
void SharedMemoryTransportV3::heartbeatLoop() {
    while (receiving_.load()) {
        // 1. 更新自己的心跳
        registry_.updateHeartbeat(node_id_);
        
        if (my_shm_) {
            my_shm_->header.last_heartbeat.store(getCurrentTimestamp());
        }
        
        // 2. 获取清理前的节点列表
        std::vector<NodeInfo> nodes_before = registry_.getAllNodes();
        
        // 3. 清理过期节点（超过3秒无心跳）
        int cleaned = registry_.cleanupStaleNodes(3000);
        
        // 4. 对比找出被删除的节点
        if (cleaned > 0 && node_impl_) {
            auto nodes_after = registry_.getAllNodes();
            
            for (const auto& node : nodes_before) {
                if (node.node_id == node_id_) continue;
                
                bool found = false;
                for (const auto& n : nodes_after) {
                    if (n.node_id == node.node_id) {
                        found = true;
                        break;
                    }
                }
                
                // 节点已移除，触发NODE_LEFT事件
                if (!found) {
                    NEXUS_DEBUG("SHM-V3") << "Heartbeat timeout: " 
                                          << node.node_id;
                    node_impl_->handleNodeEvent(node.node_id, false);
                }
            }
        }
        
        // 5. 清理过期的入站队列
        cleanupStaleQueues();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
```

### 4.4 Node注销机制

```cpp
SharedMemoryTransportV3::~SharedMemoryTransportV3() {
    stopReceiving();
    
    // 1. 断开所有远程连接
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& pair : remote_connections_) {
            if (pair.second.shm_ptr && pair.second.shm_ptr != MAP_FAILED) {
                munmap(pair.second.shm_ptr, sizeof(NodeSharedMemory));
            }
            if (pair.second.shm_fd >= 0) {
                close(pair.second.shm_fd);
            }
        }
        remote_connections_.clear();
    }
    
    // 2. 从Registry注销
    if (initialized_) {
        registry_.unregisterNode(node_id_);
    }
    
    // 3. 销毁自己的共享内存
    if (my_shm_ptr_ && my_shm_ptr_ != MAP_FAILED) {
        munmap(my_shm_ptr_, sizeof(NodeSharedMemory));
    }
    if (my_shm_fd_ >= 0) {
        close(my_shm_fd_);
    }
    if (!my_shm_name_.empty()) {
        shm_unlink(my_shm_name_.c_str());
    }
}
```

**完整生命周期**：

```
1. Node启动
   ├─ 创建NodeSharedMemory (/librpc_node_PID_HASH)
   ├─ 注册到Registry
   ├─ 设置ready标志
   └─ 启动receiveLoop和heartbeatLoop

2. 发现其他Node
   ├─ queryRemoteServices()广播查询
   ├─ 其他节点回复SERVICE_REGISTER
   └─ 自动建立连接（懒惰连接）

3. 运行时维护
   ├─ 每500ms更新心跳
   ├─ 清理过期节点（>3s无心跳）
   └─ 清理过期入站队列

4. Node退出
   ├─ stopReceiving()
   ├─ 断开所有远程连接
   ├─ 从Registry注销
   └─ 删除NodeSharedMemory
```

---

## 5. 服务发现机制

### 5.1 服务注册表架构

LibRPC使用**两层服务注册表**：

1. **GlobalRegistry**（进程内）：存储本进程内所有节点的服务
2. **跨进程服务同步**：通过消息广播同步服务信息

```
┌─────────────────────────────────────────────────────┐
│              GlobalRegistry (单例)                   │
│  ┌───────────────────────────────────────────────┐  │
│  │ nodes_: map<node_id, shared_ptr<NodeImpl>>   │  │
│  │ services_: map<group, vector<ServiceDesc>>   │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
         ▲                               ▲
         │                               │
    ┌────┴────┐                    ┌────┴────┐
    │ Node A  │                    │ Node B  │
    │(进程内) │                    │(进程内) │
    └─────────┘                    └─────────┘
         │                               │
         │      SharedMemory消息         │
         └───────────────────────────────┘
              SERVICE_REGISTER
              SERVICE_UNREGISTER
```

### 5.2 服务注册流程

#### 5.2.1 本地注册

```cpp
void NodeImpl::registerService(const ServiceDescriptor& svc) {
    // 1. 注册到GlobalRegistry（进程内）
    GlobalRegistry::instance().registerService(svc.group, svc);
    
    // 2. 通知进程内其他节点
    auto nodes = getAllNodes();
    for (auto& node : nodes) {
        if (node && node->getNodeId() != node_id_) {
            node->handleServiceUpdate(node_id_, svc, true);
        }
    }
    
    // 3. 广播到远程节点（跨进程）
    broadcastServiceUpdate(svc, true);
}
```

#### 5.2.2 跨进程广播

```cpp
void NodeImpl::broadcastServiceUpdate(const ServiceDescriptor& svc, 
                                     bool is_register) {
    if (!shm_transport_v3_ || !shm_transport_v3_->isInitialized()) {
        return;
    }
    
    // 序列化服务描述
    std::vector<uint8_t> payload = serializeService(svc);
    
    // 构建消息包
    MessageType msg_type = is_register ? MessageType::SERVICE_REGISTER 
                                       : MessageType::SERVICE_UNREGISTER;
    
    std::vector<uint8_t> packet = MessageBuilder::build(
        node_id_, svc.group, svc.topic, 
        payload.data(), payload.size(), 
        0, msg_type);
    
    // 广播到所有节点
    shm_transport_v3_->broadcast(packet.data(), packet.size());
}
```

### 5.3 服务发现流程

#### 5.3.1 主动查询

```cpp
void NodeImpl::queryRemoteServices() {
    if (!shm_transport_v3_) return;
    
    // 发送空的SERVICE_REGISTER消息作为查询
    // payload_len = 0 表示"请告诉我你的服务"
    std::vector<uint8_t> query_packet = MessageBuilder::build(
        node_id_, "", "", nullptr, 0, 0, 
        MessageType::SERVICE_REGISTER);
    
    // 广播查询（触发其他节点回复）
    shm_transport_v3_->broadcast(query_packet.data(), query_packet.size());
}
```

#### 5.3.2 响应查询

```cpp
void NodeImpl::handleServiceMessage(const std::string& from_node,
                                   const std::string& group,
                                   const std::string& topic,
                                   const uint8_t* payload,
                                   size_t payload_len) {
    if (payload_len == 0) {
        // 收到查询请求，回复所有服务
        
        if (group.empty()) {
            // 查询所有组的服务
            auto all_groups = GlobalRegistry::instance().getAllGroups();
            for (const auto& g : all_groups) {
                auto services = GlobalRegistry::instance().getServices(g);
                for (const auto& svc : services) {
                    broadcastServiceUpdate(svc, true);
                }
            }
        } else {
            // 查询特定组的服务
            auto services = GlobalRegistry::instance().getServices(group);
            for (const auto& svc : services) {
                broadcastServiceUpdate(svc, true);
            }
        }
    } else {
        // 收到服务注册/注销
        ServiceDescriptor svc;
        deserializeService(payload, payload_len, svc);
        
        MessageType msg_type = /* 从消息中提取 */;
        bool is_register = (msg_type == MessageType::SERVICE_REGISTER);
        
        if (is_register) {
            // 注册服务
            GlobalRegistry::instance().registerService(svc.group, svc);
        } else {
            // 注销服务
            GlobalRegistry::instance().unregisterService(svc.group, svc);
        }
        
        // 通知订阅者
        notifyServiceUpdate(from_node, svc, is_register);
    }
}
```

### 5.4 订阅与通知

#### 5.4.1 订阅服务

```cpp
Error NodeImpl::subscribe(const std::string& group, 
                         const std::string& topic,
                         Callback callback) {
    if (!callback) {
        return INVALID_ARG;
    }
    
    // 1. 注册回调
    std::string key = group + "/" + topic;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        subscribers_[key].push_back(callback);
    }
    
    // 2. 广播SUBSCRIBE消息（通知其他节点）
    std::vector<uint8_t> packet = MessageBuilder::build(
        node_id_, group, topic, nullptr, 0, 0, 
        MessageType::SUBSCRIBE);
    
    if (shm_transport_v3_) {
        shm_transport_v3_->broadcast(packet.data(), packet.size());
    }
    
    return NO_ERROR;
}
```

#### 5.4.2 发布消息

```cpp
Error NodeImpl::publish(const std::string& group,
                       const std::string& topic,
                       const uint8_t* data, size_t size) {
    // 1. 进程内发布（同步调用）
    std::string key = group + "/" + topic;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        auto it = subscribers_.find(key);
        if (it != subscribers_.end()) {
            for (auto& cb : it->second) {
                cb(data, size, node_id_);
            }
        }
    }
    
    // 2. 跨进程发布（异步消息）
    if (shm_transport_v3_ && shm_transport_v3_->isInitialized()) {
        std::vector<uint8_t> packet = MessageBuilder::build(
            node_id_, group, topic, data, size, 0, 
            MessageType::DATA);
        
        // 广播到所有订阅者
        int sent = shm_transport_v3_->broadcast(packet.data(), packet.size());
        
        return (sent > 0) ? NO_ERROR : NETWORK_ERROR;
    }
    
    return NO_ERROR;
}
```

### 5.5 服务发现时序图

```
Node A启动                  Node B (已运行)               Node C (已运行)
   │                              │                            │
   ├─ 1. initialize()            │                            │
   │     └─ registry.register()  │                            │
   │                              │                            │
   ├─ 2. queryRemoteServices()   │                            │
   │     └─ broadcast(SERVICE_REGISTER, payload_len=0)        │
   │                              │                            │
   │     ┌────────────────────────┴─────────────────────┐     │
   │     │ 收到查询请求                                  │     │
   │     │                                               │     │
   │     ├─ 3. handleServiceMessage()                   │     │
   │     │     └─ 回复所有服务                          │     │
   │     │                                               │     │
   │◄────┤ SERVICE_REGISTER(service1)                   │     │
   │◄────┤ SERVICE_REGISTER(service2)                   │     │
   │     └───────────────────────────────────────────────┘     │
   │                              │                            │
   │     ┌──────────────────────────────────────────────┬─────┘
   │     │ 同样回复                                      │
   │     │                                               │
   │◄────┤ SERVICE_REGISTER(service3)                   │
   │     └───────────────────────────────────────────────┘
   │                              │                            │
   ├─ 4. 注册自己的服务          │                            │
   │     registerService(svc)     │                            │
   │     └─ broadcast(SERVICE_REGISTER, payload=svc)           │
   │                              │                            │
   │     ┌────────────────────────┴─────────────────────┬─────┘
   │     │ 收到新服务注册                                │
   │     │                                               │
   │     ├─ 5. handleServiceMessage()                   │
   │     │     └─ GlobalRegistry.register(svc)          │
   │     └───────────────────────────────────────────────┘
   │                              │                            │
   ▼                              ▼                            ▼
 完成发现                      完成发现                     完成发现
```

### 5.6 与共享内存的关系总结

| 机制 | 使用的共享内存 | 作用 |
|-----|--------------|------|
| **Node注册** | `/librpc_registry` | 存储所有Node的元信息（node_id, shm_name, pid, 心跳） |
| **Node连接** | `/librpc_node_PID_HASH` | 每个Node的入站队列数组，用于接收消息 |
| **服务注册** | GlobalRegistry（进程内）+ 消息广播 | 进程内用内存，跨进程用消息同步 |
| **服务发现** | 消息广播（通过共享内存传输） | queryRemoteServices触发广播查询 |
| **消息传输** | InboundQueue（SPSC队列） | 实际的消息数据传输 |

**关键设计**：
1. ✅ **Registry轻量**：仅存储发现信息，不存储消息数据
2. ✅ **分离关注点**：Node发现用Registry，消息传输用Queue
3. ✅ **懒惰连接**：首次发送时才映射对方的共享内存
4. ✅ **服务同步**：通过消息广播，避免中心化服务注册表
5. ✅ **心跳维护**：自动检测节点退出，清理过期连接

---

## 6. 通知机制设计

### 4.1 双机制对比

| 特性 | Condition Variable | Semaphore |
|------|-------------------|-----------|
| **CPU占用** | 5.7% | 5.9% |
| **丢包率** | 0.027% | 0% |
| **优点** | 更通用，跨平台 | 更简单，QNX原生 |
| **缺点** | 需要mutex开销 | 部分平台不支持进程间 |
| **适用场景** | 通用场景 | QNX/嵌入式系统 |

### 6.2 Condition Variable方案

**发送端**：
```cpp
// 批量通知优化
uint32_t prev = queue->pending_msgs.fetch_add(1, 
                    std::memory_order_release);
if (prev == 0) {
    // 只在0→1时signal，避免过度唤醒
    pthread_cond_signal(&queue->notify_cond);
}
```

**接收端**：
```cpp
pthread_mutex_lock(&wait_queue->notify_mutex);

// 双重检查避免信号丢失
if (wait_queue->pending_msgs.load(std::memory_order_acquire) == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += timeout_ms * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    // 阻塞等待（真正的等待，不轮询）
    pthread_cond_timedwait(&wait_queue->notify_cond, 
                           &wait_queue->notify_mutex, &ts);
}

pthread_mutex_unlock(&wait_queue->notify_mutex);
```

**优化要点**：
1. ✅ 批量通知（0→1才触发）
2. ✅ 双重检查（避免信号丢失）
3. ✅ 自适应超时（5ms/50ms）
4. ✅ 真正的阻塞等待（非轮询）

### 6.3 Semaphore方案

**发送端**：
```cpp
// 批量通知优化（与CV相同）
uint32_t prev = queue->pending_msgs.fetch_add(1, 
                    std::memory_order_release);
if (prev == 0) {
    sem_post(&queue->notify_sem);
}
```

**接收端**：
```cpp
struct timespec timeout;
clock_gettime(CLOCK_REALTIME, &timeout);
timeout.tv_nsec += timeout_ms * 1000000L;
if (timeout.tv_nsec >= 1000000000) {
    timeout.tv_sec++;
    timeout.tv_nsec -= 1000000000;
}

// 阻塞等待（真正的等待，不轮询）
sem_timedwait(&active_queues[0]->notify_sem, &timeout);
```

**优化要点**：
1. ✅ 批量通知（同CV）
2. ✅ 真正的等待（sem_timedwait）
3. ✅ 自适应超时（同CV）
4. ✅ 无mutex开销（略快）

### 6.4 已移除的FIFO方案

**移除原因**：
- ❌ CPU占用较高（轮询开销）
- ❌ 可靠性问题（信号可能丢失）
- ❌ 复杂度高（epoll + FIFO管理）
- ❌ 性能不如CV/Semaphore

---

## 7. 性能优化设计

### 7.1 批量通知优化

**问题**：每次发送都触发通知，浪费CPU

**原方案**（每次通知）：
```cpp
queue->pending_msgs++;
pthread_cond_signal(&queue->notify_cond);  // 每次都signal
```

**优化方案**（批量通知）：
```cpp
uint32_t prev = queue->pending_msgs.fetch_add(1, ...);
if (prev == 0) {
    pthread_cond_signal(&queue->notify_cond);  // 只在0→1时通知
}
```

**效果**：
- CPU从8.6%降到5.7%（⬇️34%）
- 减少60%的唤醒操作
- 保持相同延迟

### 7.2 缓存活跃队列

**问题**：每次循环遍历MAX_INBOUND_QUEUES（64个）

**原方案**（每次遍历）：
```cpp
for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
    InboundQueue& q = my_shm_->queues[i];
    if ((q.flags & 0x3) == 0x3) {
        // 处理消息
    }
}
```

**优化方案**（缓存列表）：
```cpp
std::vector<InboundQueue*> active_queues;  // 缓存活跃队列

// 检测num_queues变化时才刷新
uint32_t current = my_shm_->header.num_queues.load();
if (current != cached_num_queues || 
    queue_refresh_counter >= 100) {
    active_queues.clear();
    for (uint32_t i = 0; i < MAX_INBOUND_QUEUES; ++i) {
        if ((queues[i].flags & 0x3) == 0x3) {
            active_queues.push_back(&queues[i]);
        }
    }
    cached_num_queues = current;
    queue_refresh_counter = 0;
}

// 仅遍历活跃队列
for (auto* q : active_queues) {
    // 处理消息
}
```

**效果**：
- 减少90%的队列遍历
- 降低atomic load操作
- 适合多队列场景

### 7.3 Flags安全验证

**问题**：缓存的队列可能在遍历时失效

**解决方案**：
```cpp
for (auto* q : active_queues) {
    // 每次访问前验证flags
    uint32_t flags = q->flags.load(std::memory_order_relaxed);
    if ((flags & 0x3) != 0x3) {
        continue;  // 跳过失效队列
    }
    
    // 安全处理消息
    processMessages(q);
}
```

**双重保护**：
1. num_queues变化时刷新列表
2. 遍历时验证flags

### 7.4 自适应超时

**问题**：固定超时无法平衡CPU和延迟

**优化方案**：
```cpp
int consecutive_empty_loops = 0;
const int ADAPTIVE_THRESHOLD = 100;

// 根据负载动态调整
int timeout_ms = (consecutive_empty_loops > ADAPTIVE_THRESHOLD) 
                 ? 50  // 空闲：长超时降低CPU
                 : 5;  // 繁忙：短超时降低延迟

if (has_messages) {
    consecutive_empty_loops = 0;  // 重置计数
} else {
    consecutive_empty_loops++;
}
```

**效果**：
- 空闲时CPU降低70%
- 繁忙时延迟保持<10μs
- 自动适应负载变化

---

## 8. 资源管理设计

### 8.1 双重清理机制

**1. 引用计数清理（正常退出）**：

```cpp
~SharedMemoryTransportV3() {
    if (!my_shm_) return;
    
    // 减少引用计数
    int32_t prev = my_shm_->header.ref_count.fetch_sub(1);
    
    if (prev == 1) {
        // 最后一个引用，删除共享内存
        shm_unlink(my_shm_name_.c_str());
        NEXUS_DEBUG("SHM-V3") << "Deleted shared memory: " 
                               << my_shm_name_;
    }
    
    munmap(my_shm_, sizeof(NodeSharedMemory));
    close(my_shm_fd_);
}
```

**2. PID检测清理（异常退出）**：

```cpp
void cleanupOrphanedMemory() {
    DIR* dir = opendir("/dev/shm");
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "librpc_node_", 12) == 0) {
            // 解析PID
            int pid = extractPID(entry->d_name);
            
            // 检查进程是否存活
            if (!isProcessAlive(pid)) {
                // 删除孤儿共享内存
                std::string path = std::string("/dev/shm/") + entry->d_name;
                shm_unlink(path.c_str());
                NEXUS_DEBUG("Cleanup") << "Removed orphaned: " << path;
            }
        }
    }
    closedir(dir);
}

bool isProcessAlive(pid_t pid) {
    return (kill(pid, 0) == 0) || (errno != ESRCH);
}
```

**3. 心跳超时检测**：

```cpp
void heartbeatLoop() {
    while (receiving_) {
        // 更新自己的心跳
        my_shm_->header.last_heartbeat.store(
            getCurrentTimestamp(), std::memory_order_release);
        
        // 检查远程节点心跳
        for (auto& [node_id, conn] : remote_connections_) {
            uint64_t last = conn.shm_ptr->header.last_heartbeat.load();
            uint64_t now = getCurrentTimestamp();
            
            if (now - last > 5000) {  // 5秒超时
                NEXUS_WARN("SHM-V3") << "Node " << node_id 
                                      << " heartbeat timeout";
                // 标记连接断开
                conn.connected = false;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
```

### 8.2 清理对比

| 退出方式 | 引用计数 | PID检测 | 心跳超时 | 结果 |
|---------|---------|---------|---------|------|
| **正常退出** | ✅ ref→0 | ✅ PID消失 | ✅ 停止更新 | 即时清理 |
| **kill -9** | ❌ 未执行 | ✅ PID消失 | ❌ 停止更新 | 下次启动清理 |
| **崩溃** | ❌ 未执行 | ✅ PID消失 | ❌ 停止更新 | 下次启动清理 |
| **hang住** | ❌ 未减少 | ✅ PID存在 | ✅ 超时检测 | 5秒后标记失效 |

---

## 9. 错误处理设计

### 9.1 错误码定义

```cpp
enum Error {
    NO_ERROR = 0,           // 成功
    INVALID_ARG = 1,        // 参数无效
    NOT_INITIALIZED = 2,    // 未初始化
    ALREADY_EXISTS = 3,     // 资源已存在
    NOT_FOUND = 4,          // 未找到
    NETWORK_ERROR = 5,      // 网络错误
    TIMEOUT = 6,            // 超时（队列满）
    UNEXPECTED_ERROR = 99   // 未预期错误
};
```

### 9.2 错误处理策略

**1. 队列满（TIMEOUT）**：

```cpp
auto err = node->publish("sensor", "temp", data, size);
if (err == librpc::TIMEOUT) {
    // 策略1：重试
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    err = node->publish(...);
    
    // 策略2：丢弃旧消息
    // （业务决策）
    
    // 策略3：记录日志并继续
    LOG_WARN << "Queue full, message dropped";
}
```

**2. 连接失败（NOT_FOUND）**：

```cpp
if (err == librpc::NOT_FOUND) {
    // 节点未启动，等待发现
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // 自动重试连接
}
```

**3. 无法恢复的错误**：

```cpp
if (err != NO_ERROR && err != TIMEOUT && err != NOT_FOUND) {
    LOG_ERROR << "Fatal error: " << err;
    // 重新初始化或退出
}
```

---

## 10. 性能指标

### 10.1 延迟测试（1000 msg/s负载）

| 传输类型 | 消息大小 | P50延迟 | P99延迟 |
|---------|---------|---------|---------|
| 进程内通信 | 256B | <1μs | <2μs |
| 共享内存V3 (CV) | 256B | 8μs | 15μs |
| 共享内存V3 (Sem) | 256B | 8μs | 14μs |

### 10.2 吞吐量测试

| 传输类型 | 消息大小 | 吞吐量 | QPS |
|---------|---------|--------|-----|
| 共享内存V3 | 256B | ~50 MB/s | ~200,000 |
| 大数据通道 | 1MB | ~135 MB/s | ~135 |

### 10.3 CPU占用（1000 msg/s）

| 通知机制 | node0 CPU | node1 CPU | 平均CPU |
|---------|-----------|-----------|---------|
| **Condition Variable** | 5.4% | 6.0% | **5.7%** ✅ |
| **Semaphore** | 5.8% | 6.0% | **5.9%** ✅ |
| FIFO+epoll（已移除） | 4.8% | 5.2% | 5.0% ❌ |

**说明**：FIFO方案虽然CPU略低，但存在可靠性问题（丢包率0.06%），已移除。

### 10.4 内存占用

| 配置 | 队列数 | 容量 | 内存占用 | 场景 |
|-----|--------|------|---------|------|
| **最小** | 8 | 64 | ~1 MB | 资源受限 |
| **默认** | 32 | 256 | **33 MB** | **推荐** ✅ |
| **标准** | 64 | 256 | 33 MB | 高并发 |
| **最大** | 64 | 1024 | 132 MB | 高吞吐 |

**计算公式**：
```
Memory = max_inbound_queues × queue_capacity × MESSAGE_SIZE
默认 = 32 × 256 × 2048 = 16.8 MB (单节点RX)
     + Registry (4MB) + 其他开销
     ≈ 33 MB
```

### 10.5 丢包率（30秒稳定性测试）

| 方案 | 发送消息 | 接收消息 | 丢包率 |
|------|---------|---------|--------|
| CV方案 | 27,759 | 27,739 | **0.072%** ✅ |
| Semaphore方案 | 27,744 | 27,744 | **0%** ✅ |

---

## 11. 设计决策与权衡

### 11.1 为什么移除FIFO方案？

**决策**：移除FIFO+epoll通知机制

**原因**：
1. **可靠性问题**：边缘触发模式下信号可能丢失
2. **复杂度高**：需要管理FIFO文件、epoll实例
3. **性能不佳**：仍需轮询兜底，CPU优势不明显
4. **维护成本**：三种机制维护成本高

**结果**：
- ✅ 代码减少300行
- ✅ 简化架构，仅保留两种机制
- ✅ 性能保持（CV 5.7%、Sem 5.9%）
- ✅ 可靠性提升（丢包率<0.1%）

### 11.2 为什么选择SPSC队列？

**决策**：使用单生产者单消费者队列

**优点**：
- ✅ 无锁实现（仅atomic操作）
- ✅ 性能极致（无mutex竞争）
- ✅ 缓存友好（连续内存）

**缺点**：
- ❌ 不支持多写者/多读者
- ❌ 每对节点需要独立队列

**权衡**：IPC场景通常是点对点通信，SPSC完全满足需求

### 11.3 为什么提供双通知机制？

**决策**：同时支持Condition Variable和Semaphore

**理由**：
1. **跨平台兼容**：CV更通用，Sem在QNX上更原生
2. **性能相当**：两者CPU占用差异<0.5%
3. **用户选择**：根据平台和习惯选择

**实现**：通过NotifyMechanism枚举在初始化时选择

### 11.4 为什么使用批量通知？

**决策**：pending_msgs从0→1才触发通知

**分析**：
- **高频发送场景**：连续发送10条消息
  - 无优化：10次signal → 10次唤醒
  - 批量优化：1次signal → 1次唤醒处理10条

**效果**：
- ✅ 减少60%的唤醒操作
- ✅ CPU从8.6%降到5.7%
- ✅ 保持相同延迟（接收端批量处理）

### 11.5 为什么使用缓存队列？

**决策**：缓存活跃队列列表，定期刷新

**分析**：
- 典型场景：3-5个活跃节点，MAX_INBOUND_QUEUES=64
- 每次循环遍历64个队列浪费CPU

**优化**：
- 缓存5个活跃队列
- 每100次循环刷新一次
- num_queues变化时立即刷新

**效果**：
- ✅ 减少90%的队列遍历
- ✅ 降低atomic load操作
- ✅ 适应动态节点变化

---

## 10. 未来优化方向

### 10.1 短期优化（v3.1）

1. **自适应队列容量**
   - 根据消息频率动态调整队列大小
   - 目标：降低内存占用20%

2. **零拷贝大数据**
   - 优化LargeDataChannel性能
   - 目标：吞吐量 >200 MB/s

3. **更细粒度的流控**
   - 每队列独立流控策略
   - 目标：丢包率 <0.01%

### 10.2 中期优化（v4.0）

1. **支持多写者队列**
   - MPSC队列实现
   - 适用于多线程发送场景

2. **GPU内存支持**
   - 支持CUDA/OpenCL共享内存
   - 用于视觉处理场景

3. **网络传输优化**
   - RDMA支持（跨主机零拷贝）
   - 用于分布式部署

---

**文档版本**: 1.0  
**最后更新**: 2025-11-28  
**作者**: LibRPC开发团队
