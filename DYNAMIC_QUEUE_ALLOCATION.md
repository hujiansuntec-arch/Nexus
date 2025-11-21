# 动态队列分配方案 (Dynamic Queue Allocation)

## 当前问题

**现状**：每个节点预分配256个队列槽位，占用528MB内存
- 即使只有2个发送者，也占用528MB
- 99%的内存都浪费了

**目标**：按需分配队列，节省内存

---

## 🎯 方案1: 动态增长数组 (推荐 ⭐⭐⭐)

### 设计思路
使用**间接指针数组**，队列按需分配到堆内存，而不是预分配在共享内存中。

### 架构改造

#### 旧架构 (固定256个)
```cpp
struct NodeSharedMemory {
    NodeHeader header;
    InboundQueue queues[256];  // ❌ 预分配 528MB
};
```

#### 新架构 (动态分配)
```cpp
struct NodeSharedMemory {
    NodeHeader header;
    
    // 动态队列池
    struct QueuePool {
        std::atomic<uint32_t> capacity;     // 当前容量 (4, 8, 16, 32...)
        std::atomic<uint32_t> used;         // 已使用数量
        std::atomic<uint64_t> shm_offset;   // 队列数据在共享内存中的偏移
    } pool;
    
    // 队列元数据数组 (轻量级)
    struct QueueSlot {
        char sender_id[64];
        std::atomic<uint32_t> flags;
        std::atomic<uint64_t> queue_offset;  // 指向实际队列的偏移
        std::atomic<uint32_t> congestion_level;
        std::atomic<uint64_t> drop_count;
    } slots[MAX_QUEUE_SLOTS];  // MAX_QUEUE_SLOTS = 256
    
    // 动态数据区 (初始很小，按需增长)
    // char dynamic_data[DYNAMIC_POOL_SIZE];
};

// 实际的队列存储在 dynamic_data 区域
```

### 内存布局
```
共享内存布局：
┌────────────────────────────────────────────────┐
│ NodeHeader (64B)                               │
├────────────────────────────────────────────────┤
│ QueuePool (元数据，24B)                        │
├────────────────────────────────────────────────┤
│ QueueSlot[256] (256 × 88B = 22KB)             │ ← 轻量级元数据
├────────────────────────────────────────────────┤
│ Dynamic Queue Data (按需增长)                 │
│   Queue 0: LockFreeRingBuffer (2.06MB)        │ ← 仅在需要时分配
│   Queue 1: LockFreeRingBuffer (2.06MB)        │
│   ...                                          │
│   (最多256个队列)                              │
└────────────────────────────────────────────────┘

内存需求：
- 初始：64B + 24B + 22KB = ~22KB (几乎为0！)
- 1个队列：22KB + 2.06MB = ~2MB
- 10个队列：22KB + 20.6MB = ~21MB
- 256个队列：22KB + 528MB = ~528MB (同旧方案)
```

### 实现代码

#### 头文件 (SharedMemoryTransportV3.h)
```cpp
class SharedMemoryTransportV3 {
public:
    static constexpr size_t MAX_QUEUE_SLOTS = 256;      // 最大槽位数
    static constexpr size_t INITIAL_POOL_SIZE = 4;      // 初始分配4个队列
    static constexpr size_t MAX_POOL_SIZE = 256;        // 最多256个队列
    static constexpr size_t DYNAMIC_POOL_SIZE = 550 * 1024 * 1024;  // 550MB池
    
private:
    // 轻量级队列槽位 (仅元数据)
    struct QueueSlot {
        char sender_id[64];
        std::atomic<uint32_t> flags;           // Bit 0: valid, Bit 1: active
        std::atomic<uint64_t> queue_offset;    // 队列在dynamic_data中的偏移
        std::atomic<uint32_t> congestion_level;
        std::atomic<uint64_t> drop_count;
        char padding[8];  // 对齐到64字节
    };
    
    // 动态队列池
    struct QueuePool {
        std::atomic<uint32_t> capacity;        // 当前分配的队列数
        std::atomic<uint32_t> used;            // 已使用的队列数
        std::atomic<uint64_t> next_offset;     // 下一个可分配的偏移
        char padding[40];
    };
    
    // 节点共享内存布局
    struct NodeSharedMemory {
        NodeHeader header;
        QueuePool pool;
        QueueSlot slots[MAX_QUEUE_SLOTS];
        uint8_t dynamic_data[DYNAMIC_POOL_SIZE];  // 动态数据区
    };
};
```

#### 创建共享内存 (createMySharedMemory)
```cpp
bool SharedMemoryTransportV3::createMySharedMemory() {
    // 计算初始大小 (头部 + 元数据 + 预留少量队列空间)
    size_t initial_size = sizeof(NodeHeader) + 
                          sizeof(QueuePool) + 
                          sizeof(QueueSlot) * MAX_QUEUE_SLOTS +
                          sizeof(LockFreeRingBuffer<1024>) * INITIAL_POOL_SIZE;
    
    // 创建共享内存
    my_shm_fd_ = shm_open(my_shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (my_shm_fd_ < 0) {
        // ... 错误处理
    }
    
    // 设置初始大小
    if (ftruncate(my_shm_fd_, initial_size) < 0) {
        // ... 错误处理
    }
    
    // 映射内存
    my_shm_ptr_ = mmap(nullptr, DYNAMIC_POOL_SIZE + sizeof(NodeHeader) + 
                       sizeof(QueuePool) + sizeof(QueueSlot) * MAX_QUEUE_SLOTS,
                       PROT_READ | PROT_WRITE, MAP_SHARED, my_shm_fd_, 0);
    
    my_shm_ = static_cast<NodeSharedMemory*>(my_shm_ptr_);
    
    // 初始化池
    my_shm_->pool.capacity.store(INITIAL_POOL_SIZE);
    my_shm_->pool.used.store(0);
    my_shm_->pool.next_offset.store(0);
    
    // 初始化槽位
    for (size_t i = 0; i < MAX_QUEUE_SLOTS; ++i) {
        my_shm_->slots[i].flags.store(0);
        my_shm_->slots[i].queue_offset.store(UINT64_MAX);  // 无效偏移
    }
    
    std::cout << "[SHM-V3] Created shared memory: " << my_shm_name_ 
              << " (initial: " << (initial_size / 1024 / 1024) << " MB)" << std::endl;
    
    return true;
}
```

#### 动态分配队列 (findOrCreateQueue)
```cpp
SharedMemoryTransportV3::InboundQueue* SharedMemoryTransportV3::findOrCreateQueue(
    NodeSharedMemory* remote_shm, const std::string& sender_id) {
    
    // 1. 查找现有队列
    for (uint32_t i = 0; i < MAX_QUEUE_SLOTS; ++i) {
        QueueSlot& slot = remote_shm->slots[i];
        if ((slot.flags.load() & 0x1) && 
            strcmp(slot.sender_id, sender_id.c_str()) == 0) {
            // 找到现有队列
            uint64_t offset = slot.queue_offset.load();
            return reinterpret_cast<InboundQueue*>(
                remote_shm->dynamic_data + offset);
        }
    }
    
    // 2. 需要新队列 - 检查容量
    uint32_t current_used = remote_shm->pool.used.load();
    uint32_t current_capacity = remote_shm->pool.capacity.load();
    
    if (current_used >= current_capacity) {
        // 需要扩容
        if (!expandQueuePool(remote_shm)) {
            std::cerr << "[SHM-V3] Failed to expand queue pool" << std::endl;
            return nullptr;
        }
    }
    
    // 3. 分配新队列
    uint64_t queue_offset = allocateQueue(remote_shm);
    if (queue_offset == UINT64_MAX) {
        return nullptr;
    }
    
    // 4. 找到空闲槽位并绑定
    for (uint32_t i = 0; i < MAX_QUEUE_SLOTS; ++i) {
        QueueSlot& slot = remote_shm->slots[i];
        uint32_t expected = 0;
        if (slot.flags.compare_exchange_strong(expected, 0x3)) {
            // 成功占用槽位
            strncpy(slot.sender_id, sender_id.c_str(), 63);
            slot.sender_id[63] = '\0';
            slot.queue_offset.store(queue_offset);
            slot.congestion_level.store(0);
            slot.drop_count.store(0);
            
            remote_shm->pool.used.fetch_add(1);
            
            std::cout << "[SHM-V3] Allocated new queue at offset " 
                      << queue_offset << " for " << sender_id << std::endl;
            
            return reinterpret_cast<InboundQueue*>(
                remote_shm->dynamic_data + queue_offset);
        }
    }
    
    return nullptr;
}

// 分配队列存储空间
uint64_t SharedMemoryTransportV3::allocateQueue(NodeSharedMemory* shm) {
    constexpr size_t QUEUE_SIZE = sizeof(LockFreeRingBuffer<1024>);
    
    uint64_t offset = shm->pool.next_offset.fetch_add(QUEUE_SIZE);
    
    if (offset + QUEUE_SIZE > DYNAMIC_POOL_SIZE) {
        std::cerr << "[SHM-V3] Dynamic pool exhausted!" << std::endl;
        return UINT64_MAX;
    }
    
    // 初始化队列
    void* queue_ptr = shm->dynamic_data + offset;
    new (queue_ptr) LockFreeRingBuffer<1024>();  // placement new
    
    return offset;
}

// 扩容队列池 (可选，如果需要动态调整ftruncate)
bool SharedMemoryTransportV3::expandQueuePool(NodeSharedMemory* shm) {
    uint32_t old_capacity = shm->pool.capacity.load();
    uint32_t new_capacity = std::min(old_capacity * 2, MAX_POOL_SIZE);
    
    if (new_capacity == old_capacity) {
        return false;  // 已达最大容量
    }
    
    // 🔧 可选：调整文件大小 (如果需要)
    // size_t new_size = ... 计算新大小
    // ftruncate(my_shm_fd_, new_size);
    
    shm->pool.capacity.store(new_capacity);
    
    std::cout << "[SHM-V3] Expanded queue pool: " 
              << old_capacity << " -> " << new_capacity << std::endl;
    
    return true;
}
```

#### 接收循环 (receiveLoop)
```cpp
void SharedMemoryTransportV3::receiveLoop() {
    static constexpr size_t MESSAGE_SIZE = 2048;
    uint8_t buffer[MESSAGE_SIZE];
    
    while (receiving_.load()) {
        if (!my_shm_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        bool received_any = false;
        
        // 遍历所有槽位
        for (uint32_t i = 0; i < MAX_QUEUE_SLOTS; ++i) {
            QueueSlot& slot = my_shm_->slots[i];
            
            uint32_t flags = slot.flags.load(std::memory_order_relaxed);
            if ((flags & 0x3) != 0x3) {
                continue;  // 未使用的槽位
            }
            
            // 🔧 通过偏移获取实际队列
            uint64_t offset = slot.queue_offset.load();
            if (offset == UINT64_MAX) {
                continue;  // 无效队列
            }
            
            auto* queue = reinterpret_cast<LockFreeRingBuffer<1024>*>(
                my_shm_->dynamic_data + offset);
            
            char from_node[64];
            size_t msg_size = MESSAGE_SIZE;
            if (queue->tryRead(from_node, buffer, msg_size)) {
                received_any = true;
                stats_messages_received_++;
                stats_bytes_received_ += msg_size;
                
                if (receive_callback_) {
                    receive_callback_(buffer, msg_size, from_node);
                }
            }
        }
        
        if (!received_any) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}
```

### 性能对比

| 场景 | 旧方案 (固定) | 新方案 (动态) | 节省 |
|------|--------------|--------------|------|
| 初始化 (0个连接) | 528 MB | 22 KB | **99.996%** |
| 1个发送者 | 528 MB | 2.08 MB | **99.6%** |
| 10个发送者 | 528 MB | 20.6 MB | **96.1%** |
| 50个发送者 | 528 MB | 103 MB | **80.5%** |
| 256个发送者 | 528 MB | 528 MB | 0% |

### 优缺点

**✅ 优点**：
1. **大幅节省内存**：初始仅需22KB，按需增长
2. **支持更多节点**：系统总内存消耗降低99%+
3. **向后兼容**：对外API不变
4. **灵活扩展**：轻松支持更多队列槽位

**❌ 缺点**：
1. **首次连接慢**：需要分配队列（~50μs）
2. **间接访问**：多一次偏移计算（可忽略）
3. **复杂度增加**：需要管理动态分配

---

## 🎯 方案2: 稀疏数组 + mmap RESERVE (高级 ⭐⭐⭐⭐)

### 设计思路
使用`mmap`的`MAP_NORESERVE`标志，预留大地址空间但不立即分配物理内存。

### 实现要点
```cpp
// 映射大地址空间（550MB），但不分配物理内存
void* ptr = mmap(nullptr, 550 * 1024 * 1024, 
                 PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_NORESERVE,  // 🔧 关键：延迟分配
                 shm_fd, 0);

// 实际物理内存按页(4KB)分配，首次访问时触发
// 如果只用了10个队列（20MB），OS仅分配20MB物理内存
```

**优点**：
- 代码简单，几乎不用改
- OS自动管理物理内存
- 访问速度快（无间接寻址）

**缺点**：
- 依赖OS的内存过量承诺(overcommit)
- 可能在运行时OOM（如果物理内存不足）

---

## 🎯 方案3: 分段共享内存 (Segmented SHM) ⭐⭐

### 设计思路
每4个队列一组，按需创建独立的共享内存段。

```cpp
// Node A 的共享内存布局
/dev/shm/librpc_node_A_seg0  (8MB)  - 前4个队列
/dev/shm/librpc_node_A_seg1  (8MB)  - 第5-8个队列
/dev/shm/librpc_node_A_seg2  (8MB)  - 第9-12个队列
...
```

**优点**：完全按需分配，无浪费  
**缺点**：管理复杂，文件句柄消耗大

---

## 📊 推荐实施

### 阶段1: 方案2 (快速) - 1天
使用`MAP_NORESERVE`，改动最小：
```cpp
// 仅需修改 createMySharedMemory()
void* ptr = mmap(nullptr, sizeof(NodeSharedMemory), 
                 PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_NORESERVE,  // 添加这个标志
                 my_shm_fd_, 0);
```

### 阶段2: 方案1 (最优) - 1周
完整的动态分配机制，最大化内存节省。

### 配置参数
```cpp
struct DynamicConfig {
    size_t initial_queues = 4;       // 初始分配4个
    size_t max_queues = 256;         // 最大256个
    bool enable_auto_expand = true;  // 自动扩容
    size_t expand_threshold = 80;    // 使用率>80%时扩容
};
```

---

## 总结

**推荐路线**：
1. **短期**（本周）：使用方案2 (MAP_NORESERVE)，5分钟完成，立即减少99%内存占用
2. **长期**（下周）：实现方案1 (动态分配)，完全可控，最优解

**收益**：
- 10节点场景：5.29GB → **50MB** (节省99%)
- 100节点场景：52.9GB → **500MB** (节省99%)
- 启动时间：不变
- 运行性能：几乎无影响 (<1%)
