# 动态共享内存分配设计方案

## 背景

当前SharedMemoryTransportV2使用单个大型共享内存区域（132MB），包含所有节点的队列矩阵（N×N SPSC队列）。这种设计有以下限制：

1. **固定节点数量**：MAX_NODES = 8，超过后无法添加新节点
2. **内存浪费**：即使只有2个节点，也需要分配完整的8×8队列矩阵
3. **扩展性差**：增加节点数需要重新编译

## 方案对比

### 方案A：当前设计（单一共享内存）

**架构**：
```
/dev/shm/librpc_shm_v2 (132MB)
├── ControlBlock
├── NodeEntry[8]
└── ReceiverQueues[8][8]  // 64个队列，每个2MB
    ├── Node0的队列[8]
    ├── Node1的队列[8]
    └── ...
```

**优点**：
- ✅ 简单直接，所有节点共享一个内存区域
- ✅ 一次mmap，访问快速
- ✅ 原子操作管理所有节点状态
- ✅ 节点发现简单（遍历nodes数组）

**缺点**：
- ❌ 固定节点数量限制（MAX_NODES=8）
- ❌ 内存浪费（2节点也占用132MB）
- ❌ 扩展需要重新编译
- ❌ 初始化时间长（需要初始化所有队列）

**内存占用**：
```
2节点实际使用：2×2×1024×2048 = 8MB
但分配了：8×8×1024×2048 = 132MB
浪费率：93.9%
```

---

### 方案B：每节点独立共享内存（推荐）

**架构**：
```
/dev/shm/
├── librpc_registry          // 节点注册表 (4KB)
│   ├── ControlBlock
│   └── NodeEntry[]  // 动态增长的节点列表
│
├── librpc_node_<node_id_1>  // Node1的接收队列 (动态大小)
│   └── InboundQueues[]  // 来自其他节点的队列
│
├── librpc_node_<node_id_2>  // Node2的接收队列
│   └── InboundQueues[]
│
└── ...
```

**工作流程**：

1. **节点注册**：
```cpp
// 节点启动时
1. 打开 /dev/shm/librpc_registry (如果不存在则创建)
2. 在注册表中添加自己的entry
3. 创建 /dev/shm/librpc_node_<my_id> (自己的接收队列区)
4. 扫描注册表，发现其他节点
5. 为每个已存在节点在自己的区域创建接收队列
```

2. **发送消息**：
```cpp
// Node A 发送给 Node B
1. 查找注册表，获取Node B的共享内存名称
2. 打开 /dev/shm/librpc_node_<B_id>
3. 找到 "from_A" 的队列
4. 写入消息（无锁SPSC）
```

3. **接收消息**：
```cpp
// Node B 接收
1. 打开自己的 /dev/shm/librpc_node_<B_id>
2. 轮询所有入站队列 (from_A, from_C, ...)
3. 读取消息
```

4. **节点退出**：
```cpp
1. 从注册表删除自己的entry
2. 删除 /dev/shm/librpc_node_<my_id>
3. 如果是最后一个节点，删除 /dev/shm/librpc_registry
```

**数据结构**：

```cpp
// Registry共享内存 (/dev/shm/librpc_registry)
struct RegistryEntry {
    char node_id[64];
    pid_t pid;
    std::atomic<uint64_t> last_heartbeat;
    std::atomic<uint32_t> flags;  // active, valid
    char shm_name[64];  // e.g., "librpc_node_12345"
};

struct Registry {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> max_entries;
    std::atomic<uint32_t> num_active;
    RegistryEntry entries[];  // 可变长度
};

// 每个节点的共享内存 (/dev/shm/librpc_node_<id>)
struct InboundQueue {
    char sender_id[64];
    LockFreeRingBuffer<QUEUE_CAPACITY> queue;
};

struct NodeSharedMemory {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> num_queues;
    InboundQueue queues[];  // 动态数量
};
```

**优点**：
- ✅ **无节点数量限制**：可以动态添加任意数量的节点
- ✅ **内存按需分配**：2节点只用~8MB，100节点用~400MB
- ✅ **灵活扩展**：无需重新编译，运行时动态调整
- ✅ **隔离性好**：每个节点独立管理自己的接收队列
- ✅ **故障隔离**：某节点崩溃不影响其他节点的内存区域

**缺点**：
- ❌ 实现复杂度增加
- ❌ 需要管理多个共享内存区域
- ❌ 节点发现需要扫描注册表
- ❌ 首次连接需要动态创建队列

**内存占用对比**：

| 节点数 | 方案A (固定) | 方案B (动态) | 节省 |
|--------|-------------|--------------|------|
| 2      | 132 MB      | 8 MB         | 93.9% |
| 4      | 132 MB      | 32 MB        | 75.8% |
| 8      | 132 MB      | 132 MB       | 0%    |
| 16     | ❌ 不支持   | 528 MB       | ✅ 可用 |
| 32     | ❌ 不支持   | 2.1 GB       | ✅ 可用 |

---

### 方案C：混合方案（小规模固定 + 大规模动态）

**策略**：
- 前8个节点：使用方案A（单一共享内存）
- 超过8个节点：切换到方案B（动态分配）

**优点**：
- 小规模场景简单高效
- 大规模场景灵活扩展

**缺点**：
- 实现最复杂
- 维护两套代码路径

---

## 推荐方案：方案B（每节点独立共享内存）

### 实现步骤

#### Phase 1: 核心架构重构

1. **创建 SharedMemoryRegistry 类**
```cpp
class SharedMemoryRegistry {
    bool registerNode(const std::string& node_id, const std::string& shm_name);
    bool unregisterNode(const std::string& node_id);
    std::vector<NodeInfo> getAllNodes();
    NodeInfo findNode(const std::string& node_id);
};
```

2. **重构 SharedMemoryTransportV3**
```cpp
class SharedMemoryTransportV3 {
    // 节点自己的接收队列区域
    NodeSharedMemory* my_shm_;
    
    // 到其他节点的连接（按需建立）
    std::map<std::string, RemoteNodeConnection> remote_nodes_;
    
    struct RemoteNodeConnection {
        void* shm_ptr;          // 目标节点的共享内存
        InboundQueue* my_queue; // 在目标节点区域中，属于我的发送队列
        int shm_fd;
    };
};
```

#### Phase 2: 动态队列管理

1. **按需创建队列**
```cpp
bool connectToNode(const std::string& target_node_id) {
    // 1. 从注册表获取目标节点的共享内存名称
    NodeInfo target = registry_.findNode(target_node_id);
    
    // 2. 打开目标节点的共享内存
    int fd = shm_open(target.shm_name.c_str(), O_RDWR, 0666);
    void* ptr = mmap(...);
    
    // 3. 在目标节点的队列列表中找到/创建 "from_me" 的队列
    InboundQueue* queue = findOrCreateQueue(ptr, my_node_id_);
    
    // 4. 缓存连接
    remote_nodes_[target_node_id] = {ptr, queue, fd};
}
```

2. **消息发送优化**
```cpp
bool send(const std::string& dest_node_id, const uint8_t* data, size_t size) {
    // 快速路径：已经连接
    auto it = remote_nodes_.find(dest_node_id);
    if (it != remote_nodes_.end()) {
        return it->second.my_queue->push(data, size);
    }
    
    // 慢速路径：首次连接
    if (connectToNode(dest_node_id)) {
        return remote_nodes_[dest_node_id].my_queue->push(data, size);
    }
    
    return false;
}
```

#### Phase 3: 内存管理优化

1. **引用计数清理**
```cpp
// 在节点的共享内存中添加引用计数
struct NodeSharedMemory {
    std::atomic<uint32_t> ref_count;  // 有多少个发送者连接到我
    // ...
};

// 节点退出时
~SharedMemoryTransportV3() {
    // 1. 从所有已连接节点减少引用计数
    for (auto& conn : remote_nodes_) {
        decrementRefCount(conn.second.shm_ptr);
    }
    
    // 2. 等待我的引用计数归零
    while (my_shm_->ref_count.load() > 0) {
        std::this_thread::sleep_for(100ms);
    }
    
    // 3. 删除我的共享内存
    shm_unlink(my_shm_name_.c_str());
    
    // 4. 从注册表注销
    registry_.unregisterNode(my_node_id_);
}
```

2. **内存池复用**
```cpp
// 复用已删除节点的共享内存区域
struct RegistryEntry {
    // ...
    size_t shm_size;  // 共享内存大小
    bool reusable;    // 是否可复用
};
```

---

## 性能影响分析

### 延迟对比

| 操作 | 方案A (单一) | 方案B (动态) | 差异 |
|------|-------------|--------------|------|
| 消息发送 | ~1μs | ~1.2μs | +20% (首次连接慢，后续相同) |
| 消息接收 | ~1μs | ~1μs | 无差异 |
| 节点注册 | ~100μs | ~500μs | +400% (一次性操作) |
| 节点发现 | O(1) | O(N) | 扫描注册表 |

### 吞吐量对比

- 发送吞吐量：**相同** (~1M msg/s)
- 接收吞吐量：**相同** (~500K msg/s)
- 广播性能：**略降** (需要遍历连接表)

### 内存效率

```
2节点场景：
- 方案A: 132MB (浪费93.9%)
- 方案B: 8MB + 4KB注册表 ✅

16节点场景：
- 方案A: ❌ 不支持
- 方案B: 528MB ✅
```

---

## 迁移路径

### 向后兼容

**方案1：双版本共存**
```cpp
#ifdef USE_DYNAMIC_SHM
    using SharedMemoryTransport = SharedMemoryTransportV3;  // 动态
#else
    using SharedMemoryTransport = SharedMemoryTransportV2;  // 固定
#endif
```

**方案2：运行时选择**
```cpp
SharedMemoryTransportV3::Config config;
config.mode = Config::DYNAMIC;  // 或 FIXED_POOL

auto transport = std::make_shared<SharedMemoryTransportV3>(config);
```

### 渐进式迁移

1. **Phase 1**：实现SharedMemoryTransportV3，与V2并存
2. **Phase 2**：测试验证V3功能和性能
3. **Phase 3**：逐步迁移应用到V3
4. **Phase 4**：废弃V2，V3成为默认

---

## 潜在问题和解决方案

### 问题1：共享内存名称冲突

**问题**：多个同名节点在不同进程中启动
**解决**：
```cpp
// 使用 node_id + pid 生成唯一名称
std::string shm_name = "/librpc_node_" + node_id + "_" + std::to_string(getpid());
```

### 问题2：僵尸共享内存清理

**问题**：节点崩溃后共享内存残留
**解决**：
```cpp
// 1. 心跳检测（已有）
// 2. 启动时清理同名僵尸内存
bool initialize() {
    // 尝试打开同名共享内存
    int old_fd = shm_open(my_shm_name_.c_str(), O_RDWR, 0);
    if (old_fd >= 0) {
        // 检查PID是否还存在
        if (!procesExists(old_pid)) {
            shm_unlink(my_shm_name_.c_str());  // 清理僵尸
        }
    }
    
    // 创建新的
    shm_fd_ = shm_open(my_shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
}
```

### 问题3：注册表大小限制

**问题**：注册表预分配空间不足
**解决**：
```cpp
// 使用可扩展的注册表（类似vector）
struct Registry {
    std::atomic<uint32_t> capacity;     // 当前容量
    std::atomic<uint32_t> num_entries;  // 实际条目数
    // 当 num_entries == capacity 时，重新映射更大的区域
};
```

### 问题4：首次连接延迟

**问题**：首次发送消息需要建立连接，延迟高
**解决**：
```cpp
// 预连接策略
void warmupConnections() {
    auto nodes = registry_.getAllNodes();
    for (const auto& node : nodes) {
        connectToNode(node.node_id);  // 提前建立连接
    }
}
```

---

## 代码示例

### 发送端
```cpp
// 创建节点
auto transport = std::make_shared<SharedMemoryTransportV3>();
transport->initialize("sender_node");

// 发送消息（自动连接到接收端）
uint8_t data[] = "Hello";
transport->send("receiver_node", data, 5);
```

### 接收端
```cpp
// 创建节点
auto transport = std::make_shared<SharedMemoryTransportV3>();
transport->initialize("receiver_node");

// 设置回调
transport->setReceiveCallback([](const uint8_t* data, size_t size, const std::string& from) {
    printf("Received from %s: %.*s\n", from.c_str(), (int)size, data);
});

transport->startReceiving();
```

---

## 结论

### 推荐采用方案B的理由：

1. ✅ **灵活性**：无节点数量限制，适应各种规模场景
2. ✅ **内存效率**：按需分配，小规模场景节省93.9%内存
3. ✅ **可扩展性**：支持16、32甚至更多节点
4. ✅ **故障隔离**：节点崩溃不影响其他节点内存
5. ✅ **性能可接受**：首次连接慢，稳态性能与V2相同

### 实施建议：

1. **短期**（1-2周）：实现SharedMemoryTransportV3核心功能
2. **中期**（2-4周）：完整测试和性能调优
3. **长期**（1-2月）：生产环境验证，逐步替代V2

### 风险评估：

- **低风险**：核心SPSC队列逻辑不变
- **中风险**：注册表管理和动态连接需要仔细测试
- **缓解措施**：V2和V3共存，渐进式迁移

---

## 附录：内存布局对比图

### 方案A（当前）
```
┌─────────────────────────────────────────┐
│  /dev/shm/librpc_shm_v2 (132MB)        │
├─────────────────────────────────────────┤
│  ControlBlock                           │
│  NodeEntry[8]                           │
│  ┌───────────────────────────────────┐ │
│  │ ReceiverQueues[0] (Node0的入站)  │ │
│  │   Queue from Node0 (2MB)          │ │
│  │   Queue from Node1 (2MB)          │ │
│  │   ... (6个空队列，浪费12MB)      │ │
│  ├───────────────────────────────────┤ │
│  │ ReceiverQueues[1] (Node1的入站)  │ │
│  │   Queue from Node0 (2MB)          │ │
│  │   Queue from Node1 (2MB)          │ │
│  │   ... (6个空队列，浪费12MB)      │ │
│  ├───────────────────────────────────┤ │
│  │ ReceiverQueues[2-7] (全部浪费)   │ │
│  │   (6×16MB = 96MB 完全浪费)       │ │
│  └───────────────────────────────────┘ │
└─────────────────────────────────────────┘
总计: 132MB, 实际使用: 8MB, 浪费: 93.9%
```

### 方案B（推荐）
```
/dev/shm/
├── librpc_registry (4KB)
│   ├── Entry: Node0, shm=/librpc_node_0
│   └── Entry: Node1, shm=/librpc_node_1
│
├── librpc_node_0 (4MB)
│   ├── Queue from Node1 (2MB)
│   └── (其他队列按需添加)
│
└── librpc_node_1 (4MB)
    ├── Queue from Node0 (2MB)
    └── (其他队列按需添加)

总计: 4KB + 8MB = ~8MB
内存利用率: 100%
```

如果增加到8个节点：
```
总计: 4KB + 64×2MB = ~132MB
(与方案A相同，但可扩展到更多节点)
```

如果增加到16个节点：
```
总计: 4KB + 256×2MB = ~528MB
(方案A无法支持)
```
