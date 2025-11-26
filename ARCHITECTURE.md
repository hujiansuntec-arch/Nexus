# LibRPC 架构设计文档

**版本**: 3.0  
**日期**: 2025-11-26  
**作者**: LibRPC Team

---

## 目录

1. [概述](#1-概述)
2. [整体架构](#2-整体架构)
3. [传输层设计](#3-传输层设计)
4. [资源管理](#4-资源管理)
5. [性能优化](#5-性能优化)
6. [设计权衡](#6-设计权衡)

---

## 1. 概述

### 1.1 设计目标

LibRPC 是一个高性能进程间通信库，设计目标包括：

| 目标 | 指标 | 实现方式 |
|-----|------|---------|
| **低延迟** | <10μs（共享内存） | 无锁队列 + 零拷贝 |
| **高吞吐** | >100 MB/s（大数据） | 环形缓冲区 + 批量传输 |
| **可扩展** | 无节点数量限制 | 动态内存分配 |
| **高可用** | 异常退出恢复 | PID检测 + 引用计数 + 心跳 |
| **易用性** | 简洁API | 发布-订阅模式 |
| **低内存** | 33MB/节点（默认） | 配置化队列 + MAP_NORESERVE |

### 1.2 核心特性

```
┌──────────────────────────────────────────────────────┐
│              LibRPC 核心特性 (v3.0)                  │
├──────────────────────────────────────────────────────┤
│  ✓ 零拷贝共享内存传输（动态SPSC队列）                 │
│  ✓ 内存优化（529MB → 33MB，降低94%）                │
│  ✓ 跨平台支持（Linux + QNX）                        │
│  ✓ 动态节点发现（基于Registry）                      │
│  ✓ 三重清理机制（引用计数 + PID + 心跳）              │
│  ✓ CRC32数据完整性校验                               │
│  ✓ 流控机制（背压保护）                              │
│  ✓ 发布-订阅模式（主题分组）                         │
└──────────────────────────────────────────────────────┘
```

---

## 2. 整体架构

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│                                                               │
│   Node::publish()  |  Node::subscribe()  |  sendLargeData() │
└─────────────────────────────────────────────────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      Node Layer                              │
│                                                               │
│  • Message routing (topic-based)                             │
│  • Callback management                                       │
│  • Transport selection (inprocess vs shared memory)          │
│  • Node discovery (via Registry)                             │
│  • Heartbeat monitoring (NODE_LEFT event)                   │
└─────────────────────────────────────────────────────────────┘
                              ▼
         ┌────────────────────┼────────────────────┐
         │                    │                    │
┌────────▼─────────┐  ┌───────▼────────┐  ┌───────▼──────────┐
│  InProcess       │  │  SharedMemory  │  │  LargeData       │
│  Transport       │  │  V3 Transport  │  │  Channel         │
│                  │  │                │  │                  │
│  • Direct call   │  │  • SPSC queue  │  │  • Ring buffer   │
│  • No overhead   │  │  • Dynamic     │  │  • Zero-copy     │
│  • <1μs          │  │  • <10μs       │  │  • 135 MB/s      │
│                  │  │  • 33MB/node   │  │  • 64MB buffer   │
└──────────────────┘  └────────────────┘  └──────────────────┘
                              │
                    ┌─────────┴─────────┐
                    │                   │
            ┌───────▼──────┐    ┌───────▼──────┐
            │   Registry   │    │   Node SHM   │
            │ (Discovery)  │    │  (RX Queue)  │
            │              │    │              │
            │  /dev/shm/   │    │  /dev/shm/   │
            │  librpc_     │    │  librpc_node_│
            │  registry    │    │  <pid>_<hash>│
            └──────────────┘    └──────────────┘
```

### 2.2 组件职责

| 组件 | 职责 | 文件 |
|-----|------|------|
| **Node** | 消息路由、订阅管理、传输选择、心跳集成 | Node.h, NodeImpl.cpp |
| **InProcess Transport** | 进程内通信（直接调用） | NodeImpl.cpp |
| **SharedMemory V3** | 跨进程小消息传输、心跳监控 | SharedMemoryTransportV3.cpp |
| **LargeDataChannel** | 大数据专用通道 | LargeDataChannel.cpp |
| **Registry** | 节点注册与发现 | SharedMemoryRegistry.cpp |
| **LockFreeQueue** | 无锁环形队列 | LockFreeQueue.h |

---

## 3. 传输层设计

### 3.1 InProcess Transport

**适用场景**：同进程内的节点通信

**设计**：
```cpp
class NodeImpl {
    std::map<std::string, std::shared_ptr<NodeImpl>> inprocess_nodes_;
    
    void publish(...) {
        for (auto& [node_id, node] : inprocess_nodes_) {
            // 直接调用回调，无序列化
            node->dispatchMessage(topic, data, size);
        }
    }
};
```

**优点**：
- ✅ 零开销（直接函数调用）
- ✅ 无序列化
- ✅ 延迟 <1μs

**缺点**：
- ❌ 仅限同进程

---

### 3.2 SharedMemory V3 Transport

**适用场景**：跨进程小消息传输（<256KB）

#### 3.2.1 架构设计

```
┌────────────────────────────────────────────────────────┐
│                  /dev/shm/librpc_registry              │
│  ┌──────────────────────────────────────────────────┐  │
│  │  Node0: "app_A" -> /librpc_node_12345_abc123    │  │
│  │  Node1: "app_B" -> /librpc_node_67890_def456    │  │
│  │  ...                                             │  │
│  └──────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
                       ▲
                       │ (lookup + heartbeat)
                       │
        ┌──────────────┴──────────────┐
        │                             │
┌───────▼──────────────┐      ┌───────▼──────────────┐
│ /dev/shm/            │      │ /dev/shm/            │
│ librpc_node_12345_   │      │ librpc_node_67890_   │
│ abc123               │      │ def456               │
│                      │      │                      │
│ ┌──────────────────┐ │      │ ┌──────────────────┐ │
│ │ NodeHeader       │ │      │ │ NodeHeader       │ │
│ │  • magic         │ │      │ │  • magic         │ │
│ │  • version       │ │      │ │  • version       │ │
│ │  • owner_pid ✓   │ │      │ │  • owner_pid ✓   │ │
│ │  • heartbeat ✓   │ │      │ │  • heartbeat ✓   │ │
│ └──────────────────┘ │      │ └──────────────────┘ │
│                      │      │                      │
│ ┌──────────────────┐ │      │ ┌──────────────────┐ │
│ │ InboundQueue[0]  │ │      │ │ InboundQueue[0]  │ │
│ │  from: "app_B"   │ │      │ │  from: "app_A"   │ │
│ │  queue: SPSC     │ │      │ │  queue: SPSC     │ │
│ │  (256 slots)     │ │      │ │  (256 slots)     │ │
│ └──────────────────┘ │      │ └──────────────────┘ │
│ │ InboundQueue[1]  │ │      │ │ InboundQueue[1]  │ │
│ │  ...             │ │      │ │  ...             │ │
│ └──────────────────┘ │      │ └──────────────────┘ │
│ [最多32个队列]        │      │ [最多32个队列]        │
└──────────────────────┘      └──────────────────────┘
```

#### 3.2.2 数据结构（优化后）

```cpp
// 节点共享内存布局（v3.0优化）
struct NodeSharedMemory {
    NodeHeader header;
    InboundQueue queues[max_inbound_queues];  // 默认32，最大64
};

// 节点头部
struct NodeHeader {
    std::atomic<uint32_t> magic;           // 魔数（验证）
    std::atomic<uint32_t> version;         // 版本号
    std::atomic<uint32_t> num_queues;      // 活跃队列数
    std::atomic<uint32_t> max_queues;      // 最大队列数
    std::atomic<uint64_t> last_heartbeat;  // 心跳时间戳 ✓
    std::atomic<bool> ready;               // 就绪标志
    std::atomic<int32_t> owner_pid;        // 进程PID ✓
    char padding[31];
} __attribute__((aligned(64)));

// 入站队列（从某个发送者接收）
struct InboundQueue {
    char sender_id[64];                         // 发送者ID
    std::atomic<uint32_t> flags;                // 标志位
    LockFreeRingBuffer<QUEUE_CAPACITY> queue;   // 无锁队列（256）
    std::atomic<uint32_t> congestion_level;     // 拥塞等级
    std::atomic<uint64_t> drop_count;           // 丢包计数
    char padding[56];
} __attribute__((aligned(64)));
```

#### 3.2.3 内存优化历程

| 版本 | QUEUE_CAPACITY | MAX_INBOUND_QUEUES | 默认队列数 | 内存占用 | 优化 |
|------|---------------|-------------------|-----------|---------|------|
| **v2.5** | 1024 | 256 | 256 | **529 MB** | 基准 |
| **v2.6** | 256 | 256 | 256 | 132 MB | ↓ 75% |
| **v2.7** | 256 | 64 | 64 | 33 MB | ↓ 94% |
| **v3.0** | 256 | 64 | **32** | **33 MB** | ✓ 最终 |

**优化策略**：
1. 降低队列容量：1024 → 256（单队列 2MB → 0.5MB）
2. 限制最大队列数：256 → 64（最大132MB → 33MB）
3. 默认队列数：64 → 32（按需创建，默认占用更小）

#### 3.2.4 心跳机制与NODE_LEFT集成

**心跳监控线程**：
```cpp
void SharedMemoryTransportV3::heartbeatLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // 1. 更新自己的心跳
        header_->last_heartbeat.store(getCurrentTimestamp());
        
        // 2. 检查所有已知节点的心跳
        size_t cleaned = 0;
        for (auto it = node_shm_map_.begin(); it != node_shm_map_.end();) {
            uint64_t last_hb = it->second.header->last_heartbeat.load();
            uint64_t now = getCurrentTimestamp();
            
            // 超时5秒
            if (now - last_hb > 5000) {
                std::cout << "[ShmV3] Node timeout: " << it->first 
                          << ", last heartbeat: " << (now - last_hb) << "ms ago"
                          << std::endl;
                
                // 清理节点
                cleanupNodeConnection(it->second);
                it = node_shm_map_.erase(it);
                cleaned++;
            } else {
                ++it;
            }
        }
        
        // 3. ✓ 触发NODE_LEFT事件（集成服务发现）
        if (cleaned > 0 && node_impl_) {
            for (const auto& node_id : cleaned_nodes) {
                node_impl_->handleNodeEvent(node_id, false);  // false = left
            }
        }
    }
}
```

**集成效果**：
- ✅ 崩溃节点2-5秒内自动清理
- ✅ 服务发现自动更新（移除失效节点）
- ✅ 订阅者列表自动维护

#### 3.2.5 性能特性（v3.0）

| 特性 | 值 | 说明 |
|-----|---|------|
| **队列容量** | 256条消息 | 可配置64-1024 |
| **最大入站队列** | 64个 | 硬编码上限 |
| **默认队列数** | 32个 | 可配置8-64 |
| **延迟** | 8μs (P50) | 无锁队列 |
| **吞吐** | ~50 MB/s | 256字节消息 |
| **内存占用** | **33MB/节点** | 优化94% ✓ |
| **心跳间隔** | 1秒 | 后台线程 |
| **超时检测** | 5秒 | 自动清理 |

---

### 3.3 LargeDataChannel

**适用场景**：高频大数据传输（>1MB，最大8MB）

#### 3.3.1 架构设计

```
┌────────────────────────────────────────────────────────┐
│         /dev/shm/camera_channel (64MB)                 │
│                                                         │
│  ┌──────────────────────────────────────────────────┐  │
│  │  RingBufferControl                               │  │
│  │  ┌────────────────────────────────────────────┐  │  │
│  │  │ write_pos, read_pos, sequence              │  │  │
│  │  │ writer_heartbeat, reader_heartbeat         │  │  │
│  │  │ ref_count ✓                                │  │  │
│  │  │ writer_pid ✓, reader_pid ✓                │  │  │
│  │  │ capacity, max_block_size                   │  │  │
│  │  └────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────┘  │
│                                                         │
│  ┌──────────────────────────────────────────────────┐  │
│  │  Ring Buffer (64MB - sizeof(Control))           │  │
│  │                                                  │  │
│  │  ┌────────┐  ┌────────┐  ┌────────┐            │  │
│  │  │ Block1 │→ │ Block2 │→ │ Block3 │  ...       │  │
│  │  └────────┘  └────────┘  └────────┘            │  │
│  │                                                  │  │
│  │  Each block:                                    │  │
│  │  ┌──────────────────────────────────────────┐  │  │
│  │  │ LargeDataHeader                          │  │  │
│  │  │  • size, sequence, crc32, timestamp      │  │  │
│  │  │  • topic                                 │  │  │
│  │  └──────────────────────────────────────────┘  │  │
│  │  ┌──────────────────────────────────────────┐  │  │
│  │  │ Payload data (variable length)          │  │  │
│  │  └──────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
```

#### 3.3.2 数据结构

```cpp
// 环形缓冲区控制块
struct RingBufferControl {
    std::atomic<uint64_t> write_pos;         // 写位置
    std::atomic<uint64_t> read_pos;          // 读位置
    std::atomic<uint64_t> sequence;          // 序列号
    std::atomic<uint64_t> reader_heartbeat;  // 读端心跳
    std::atomic<uint64_t> writer_heartbeat;  // 写端心跳
    std::atomic<int32_t> ref_count;          // 引用计数 ✓
    std::atomic<int32_t> reader_pid;         // 读端PID ✓
    std::atomic<int32_t> writer_pid;         // 写端PID ✓
    uint64_t capacity;                       // 缓冲区容量
    uint32_t max_block_size;                 // 最大块大小
    uint32_t reserved;
} __attribute__((aligned(64)));

// 数据块头部
struct LargeDataHeader {
    uint64_t size;          // 数据大小
    uint64_t sequence;      // 序列号
    uint64_t timestamp;     // 时间戳
    uint32_t crc32;         // CRC32校验 ✓
    char topic[64];         // 主题名称
    char reserved[36];
} __attribute__((aligned(64)));
```

#### 3.3.3 零拷贝读取

**传统拷贝方式**（2次拷贝）：
```
Shared Memory → Temp Buffer → Application Buffer
     (copy 1)        (copy 2)
```

**零拷贝方式**（0次拷贝）：
```cpp
DataBlock block;
if (channel->tryRead(block)) {
    // block.data 直接指向共享内存
    // 无拷贝，直接访问！✓
    processData(block.data, block.header.size);
    
    // 处理完后释放
    channel->releaseBlock(block);
}
```

#### 3.3.4 流控机制

```cpp
int64_t write(const uint8_t* data, size_t size) {
    // 检查可用空间
    uint64_t available = capacity - (write_pos - read_pos);
    
    if (total_size > available) {
        // 队列满，检查读端是否存活
        if (readerDeadFor(30)) {
            // 读端已死，重置队列
            read_pos.store(write_pos.load());
        } else {
            // 返回超时错误，应用层重试
            return -1;  // TIMEOUT
        }
    }
    
    // 写入数据...
}
```

#### 3.3.5 性能特性

| 特性 | 值 | 说明 |
|-----|---|------|
| **缓冲区大小** | 64MB（默认） | 可配置128/256MB |
| **最大块大小** | 8MB | 单次传输上限 |
| **延迟** | 35μs (P50, 1MB) | 零拷贝读取 |
| **吞吐** | ~135 MB/s | 1MB块 |
| **CRC校验** | CRC32 | 数据完整性 ✓ |

---

## 4. 资源管理

### 4.1 三重清理机制（v3.0增强）

LibRPC 采用**引用计数 + PID检测 + 心跳监控**三重清理机制，确保共享内存资源不泄漏。

```
┌─────────────────────────────────────────────────────┐
│            资源清理决策树 (v3.0)                     │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
              ┌────────────────┐
              │  进程退出？    │
              └────────────────┘
                 │          │
          Normal │          │ Abnormal (kill -9 / crash)
                 ▼          ▼
        ┌────────────┐  ┌────────────────┐
        │ 析构函数   │  │ 心跳超时检测    │
        │ 执行       │  │ (2-5秒)        │
        └────────────┘  │                │
                 │      │ PID检测清理    │
                 │      │ (下次启动)      │
                 │      └────────────────┘
                 ▼          │
        ┌────────────────────────────┐
        │  ref_count--               │
        │  if (ref_count == 0)       │
        │    shm_unlink()            │
        └────────────────────────────┘
                       │
                       ▼
              ┌────────────────┐
              │  资源已释放    │
              └────────────────┘
```

### 4.2 引用计数清理

**适用场景**：正常退出（exit, return, SIGTERM）

**实现**：
```cpp
class LargeDataChannel {
    ~LargeDataChannel() {
        if (!control_) return;
        
        // 递减引用计数
        int32_t prev = control_->ref_count.fetch_sub(1, 
                                    std::memory_order_acq_rel);
        
        std::cout << "[LargeData] Destructor: " << shm_name_ 
                  << ", ref_count: " << prev << " -> " << (prev - 1) 
                  << std::endl;
        
        // 最后一个引用，删除共享内存
        if (prev == 1) {
            munmap(shm_addr_, shm_size_);
            close(shm_fd_);
            shm_unlink(shm_name_.c_str());  // ✓ 立即清理
            std::cout << "[LargeData] Last reference, unlinked: " 
                      << shm_name_ << std::endl;
        }
    }
};
```

**效果**：
- ✅ 正常退出时立即清理
- ✅ 多进程场景，最后退出的进程负责清理
- ❌ 异常退出（kill -9）时无法执行

### 4.3 心跳超时清理（v3.0新增）

**适用场景**：进程崩溃、kill -9（快速检测）

**实现**：
```cpp
void SharedMemoryTransportV3::heartbeatLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // 检测超时节点
        for (auto it = node_shm_map_.begin(); it != node_shm_map_.end();) {
            uint64_t last_hb = it->second.header->last_heartbeat.load();
            uint64_t now = getCurrentTimestamp();
            
            if (now - last_hb > 5000) {  // 5秒超时
                // 清理节点共享内存
                munmap(it->second.addr, it->second.size);
                close(it->second.fd);
                
                // 触发NODE_LEFT事件
                if (node_impl_) {
                    node_impl_->handleNodeEvent(it->first, false);
                }
                
                it = node_shm_map_.erase(it);
            } else {
                ++it;
            }
        }
    }
}
```

**效果**：
- ✅ 崩溃节点2-5秒内检测并清理
- ✅ 无需重启进程
- ✅ 自动触发服务发现更新

### 4.4 PID检测清理

**适用场景**：异常退出（kill -9, 崩溃）后重启

**实现**：
```cpp
// 首次创建通道时自动清理
std::shared_ptr<LargeDataChannel> LargeDataChannel::create(...) {
    static std::atomic<bool> g_cleanup_performed{false};
    
    if (!g_cleanup_performed.exchange(true)) {
        std::cout << "[LargeData] First channel creation, "
                  << "performing startup cleanup..." << std::endl;
        cleanupOrphanedChannels(60);  // ✓ PID检测
    }
    
    // 创建通道...
}

// 清理孤儿通道
size_t LargeDataChannel::cleanupOrphanedChannels(uint32_t timeout_seconds) {
    DIR* dir = opendir("/dev/shm");
    
    while ((entry = readdir(dir)) != nullptr) {
        // 只处理*channel*文件
        if (name.find("channel") == std::string::npos) continue;
        
        // 映射共享内存
        void* addr = mmap(...);
        RingBufferControl* control = static_cast<RingBufferControl*>(addr);
        
        // ✓ 优先级1: PID检测（最快、最准确）
        int32_t writer_pid = control->writer_pid.load();
        int32_t reader_pid = control->reader_pid.load();
        
        bool writer_alive = isProcessAlive(writer_pid);
        bool reader_alive = isProcessAlive(reader_pid);
        
        if ((writer_pid > 0 || reader_pid > 0) && 
            !writer_alive && !reader_alive) {
            // 两个进程都不存在，立即清理
            shm_unlink(name.c_str());  // ✓
            continue;
        }
        
        // ✓ 优先级2: 引用计数检测
        if (control->ref_count == 0) {
            shm_unlink(name.c_str());
            continue;
        }
        
        // ✓ 优先级3: 心跳超时检测（fallback）
        if (heartbeatTimeout(control, timeout_seconds)) {
            shm_unlink(name.c_str());
        }
    }
}

// PID存活检测
static bool isProcessAlive(int32_t pid) {
    if (pid <= 0) return false;
    
    // kill(pid, 0) 不发送信号，只检查进程是否存在
    if (kill(pid, 0) == 0) {
        return true;  // 进程存在
    }
    
    // ESRCH: 进程不存在
    // EPERM: 进程存在但无权限（也算存活）
    return errno != ESRCH;
}
```

**效果**：
- ✅ kill -9后，下次启动立即清理（<100ms）
- ✅ 精确判断（PID检测）
- ✅ 性能优化（目录扫描，非暴力尝试）

### 4.5 清理性能对比（v3.0）

| 方法 | 延迟 | 准确性 | 操作次数 | 应用场景 |
|-----|------|--------|---------|---------|
| **引用计数** | 0ms（即时） | 100% | O(1) | 正常退出 ✓ |
| **心跳超时** | 2-5s | 99% | O(1) | 崩溃检测 ✓ |
| **PID检测** | <100ms | 99.9% | O(N)文件扫描 | 启动清理 ✓ |

---

## 5. 性能优化

### 5.1 无锁队列

**SPSC队列**（Single Producer Single Consumer）：

```cpp
template<size_t Capacity>
class LockFreeRingBuffer {
private:
    alignas(64) std::atomic<uint64_t> head_;  // 写指针
    alignas(64) std::atomic<uint64_t> tail_;  // 读指针
    alignas(64) uint8_t buffer_[Capacity];    // 数据缓冲区
    
public:
    bool enqueue(const uint8_t* data, size_t size) {
        uint64_t current_head = head_.load(std::memory_order_relaxed);
        uint64_t current_tail = tail_.load(std::memory_order_acquire);
        
        // 检查空间
        uint64_t available = Capacity - (current_head - current_tail);
        if (size + sizeof(Header) > available) {
            return false;  // 队列满
        }
        
        // 写入数据（无锁！）
        // ...
        
        // 更新头指针
        head_.store(current_head + total_size, std::memory_order_release);
        return true;
    }
};
```

**关键技术**：
- ✅ Cache line对齐（避免false sharing）
- ✅ Memory order优化（relaxed/acquire/release）
- ✅ 单生产者单消费者（无竞争）

### 5.2 零拷贝

**传统方式**（2次拷贝）：
```cpp
// ❌ 拷贝方式
std::vector<uint8_t> recv_buffer(size);
shm_read(shm_addr, recv_buffer.data(), size);  // Copy 1
processData(recv_buffer);                      // Copy 2（可能）
```

**零拷贝方式**（0次拷贝）：
```cpp
// ✅ 零拷贝
DataBlock block;
channel->tryRead(block);  // block.data 直接指向共享内存

processData(block.data, block.header.size);  // 直接访问，无拷贝！✓

channel->releaseBlock(block);
```

**性能提升**：
- 1MB数据：省略2MB拷贝 → **2倍性能**
- 4MB数据：省略8MB拷贝 → **2倍性能**

### 5.3 MAP_NORESERVE 优化（Linux）

**问题**：64MB共享内存立即分配64MB物理内存

**解决**：延迟分配物理内存
```cpp
#ifndef __QNXNTO__
void* addr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_NORESERVE,  // ✓ Linux关键！
                  fd, 0);
#else
void* addr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
                  MAP_SHARED,  // QNX不支持MAP_NORESERVE
                  fd, 0);
#endif
```

**效果**：
- 虚拟内存：64MB（立即分配）
- 物理内存：按需分配（实际使用时）
- 节省内存：未使用部分不占用物理RAM
- **QNX差异**：立即分配物理内存

### 5.4 Cache Line对齐

```cpp
// ✅ Cache line对齐（64字节）
struct alignas(64) RingBufferControl {
    std::atomic<uint64_t> write_pos;  // Cache line 0
    // ...
} __attribute__((aligned(64)));

struct alignas(64) InboundQueue {
    char sender_id[64];               // Cache line 0
    std::atomic<uint32_t> flags;      // Cache line 1
    // ...
    char padding[56];                 // 填充到64字节
} __attribute__((aligned(64)));
```

**避免False Sharing**：
- 不同CPU核心访问不同的cache line
- 避免不必要的cache同步开销
- 性能提升：~10-20%

---

## 6. 设计权衡

### 6.1 内存占用 vs 队列容量

**v3.0选择**：配置化队列参数

| 方案 | 内存占用 | 队列容量 | 并发性能 | 决策 |
|-----|---------|---------|---------|------|
| **最小配置** | 8MB | 64×16 | 低 | 嵌入式 |
| **默认配置** | 33MB | 256×32 | 中 | ✅ 推荐 |
| **高性能配置** | 132MB | 1024×64 | 高 | 服务器 |

**理由**：不同场景需求不同，提供配置化选项。

### 6.2 心跳 vs PID检测

**v3.0选择**：心跳 + PID（互补）

| 方案 | 延迟 | 准确性 | 兼容性 | 决策 |
|-----|------|--------|--------|------|
| **仅心跳** | 2-5s | 99% | 高 | ⚠️ 延迟较长 |
| **仅PID** | <100ms | 99.9% | 中 | ⚠️ 需重启 |
| **心跳 + PID** | 2-5s + <100ms | 99.9% | 高 | ✅ 采用 |

**理由**：
- 心跳提供运行时检测（无需重启）
- PID提供启动时清理（快速恢复）
- 两者互补，覆盖所有场景

### 6.3 多读者 vs 单读者

**v3.0选择**：单读者（SPSC）

| 方案 | 性能 | 复杂度 | 场景 | 决策 |
|-----|------|--------|------|------|
| **SPSC** | 高（无锁） | 低 | 点对点 | ✅ 采用 |
| **SPMC** | 中（需锁） | 中 | 广播 | ❌ 按需实现 |
| **MPMC** | 低（重锁） | 高 | 通用 | ❌ 不需要 |

**理由**：大部分场景是点对点或发布-订阅（多个SPSC），无需MPMC。

### 6.4 CRC32 vs 无校验

**v3.0选择**：CRC32校验

| 方案 | 可靠性 | 性能影响 | 决策 |
|-----|--------|---------|------|
| **无校验** | 低 | 0% | ❌ 风险高 |
| **CRC32** | 中 | ~2-5% | ✅ 采用 |
| **SHA256** | 高 | ~20-30% | ❌ 过度 |

**理由**：共享内存偶尔会损坏（硬件故障、内核bug），CRC32开销小且足够。

---

## 附录

### A. 性能测试数据（v3.0）

#### A.1 延迟分布（单位：μs）

| 传输类型 | 消息大小 | P50 | P90 | P99 | P99.9 |
|---------|---------|-----|-----|-----|-------|
| InProcess | 256B | 0.5 | 0.8 | 1.2 | 2.0 |
| SharedMemory V3 | 256B | 8 | 12 | 15 | 25 |
| LargeDataChannel | 1MB | 35 | 60 | 80 | 150 |
| LargeDataChannel | 4MB | 120 | 180 | 250 | 400 |

#### A.2 吞吐量测试

| 传输类型 | 块大小 | 频率 | 吞吐量 | CPU占用 |
|---------|-------|------|--------|---------|
| SharedMemory V3 | 256B | 200K/s | 50 MB/s | 15% |
| LargeDataChannel | 1MB | 135/s | 135 MB/s | 8% |
| LargeDataChannel | 4MB | 27/s | 110 MB/s | 7% |
| LargeDataChannel | 8MB | 14/s | 112 MB/s | 6% |

### B. 内存布局详情（v3.0优化）

#### B.1 SharedMemory V3 节点（默认配置）

```
总大小：33MB（32队列 × 256容量）

┌─────────────────────────────────────┐ 0x0000
│  NodeHeader (64B)                   │
│  • magic, version, num_queues       │
│  • owner_pid, heartbeat, ready      │
├─────────────────────────────────────┤ 0x0040
│  InboundQueue[0] (0.5MB)            │
│  • sender_id, flags                 │
│  • LockFreeRingBuffer<256>          │
├─────────────────────────────────────┤ 0x080040
│  InboundQueue[1] (0.5MB)            │
├─────────────────────────────────────┤
│  ...                                │
├─────────────────────────────────────┤
│  InboundQueue[31] (0.5MB)           │
└─────────────────────────────────────┘ 33MB
```

#### B.2 LargeDataChannel

```
总大小：64MB（默认）

┌─────────────────────────────────────┐ 0x0000
│  RingBufferControl (64B)            │
│  • write_pos, read_pos, sequence    │
│  • reader_pid, writer_pid           │
│  • ref_count, capacity              │
├─────────────────────────────────────┤ 0x0040
│  Ring Buffer (64MB - 64B)           │
│                                     │
│  ┌─────────────────────────────┐   │
│  │ LargeDataHeader (128B)      │   │
│  │ Payload (variable)          │   │
│  └─────────────────────────────┘   │
│  ┌─────────────────────────────┐   │
│  │ LargeDataHeader (128B)      │   │
│  │ Payload (variable)          │   │
│  └─────────────────────────────┘   │
│  ...                                │
└─────────────────────────────────────┘ 64MB
```

---

**最后更新**: 2025-11-26  
**版本**: 3.0
