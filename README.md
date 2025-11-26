# LibRPC - 高性能进程间通信库# LibRPC - 高性能进程间通信库# LibRpc Communication Framework



[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[![C++14](https://img.shields.io/badge/C%2B%2B-14-blue.svg)](https://en.cppreference.com/w/cpp/14)

[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20QNX-lightgrey.svg)](https://www.qnx.com/)[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)A lightweight, high-performance peer-to-peer RPC communication framework supporting in-process, inter-process (lock-free shared memory), and cross-host (UDP) communication.



高性能、零拷贝的进程间通信库，专为低延迟和高吞吐量场景设计。支持发布-订阅模式、动态共享内存传输和大数据传输。[![C++14](https://img.shields.io/badge/C%2B%2B-14-blue.svg)](https://en.cppreference.com/w/cpp/14)



---[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20QNX-lightgrey.svg)](https://www.qnx.com/)## Features



## ✨ 核心特性



### 🚀 高性能通信高性能、零拷贝的进程间通信库，专为低延迟和高吞吐量场景设计。支持发布-订阅模式、共享内存传输和大数据传输。- **Peer-to-peer architecture**: All endpoints are equal nodes



- **零拷贝共享内存传输**：V3架构，动态无锁SPSC队列- **Topic-based pub/sub**: Subscribe to specific topics within message groups

- **内存优化**：默认33MB/节点（可配置8-132MB）

- **低延迟**：进程内 <1μs，共享内存 <10μs## ✨ 核心特性- **Multi-node support**: Multiple nodes can coexist in the same process (up to 8 nodes)

- **高吞吐**：共享内存 ~50 MB/s，大数据通道 ~135 MB/s

- **Triple transport mechanism**: 

### 🎯 灵活的通信模式

### 🚀 高性能通信  - In-process: Direct function calls (zero-copy, < 1μs latency)

- **发布-订阅**：主题分组、选择性投递

- **进程内通信**：同进程直接调用（零开销）- **零拷贝共享内存传输**：V3架构，无锁环形队列  - Inter-process: Lock-free shared memory SPSC queues (~500K msg/s)

- **跨进程通信**：动态共享内存（无节点数限制）

- **自动节点发现**：基于Registry的动态发现- **大数据通道**：专用通道支持高频大数据传输（>1MB）  - Cross-host: UDP broadcast/point-to-point



### 🛡️ 可靠性保证- **低延迟**：进程内 <1μs，共享内存 <10μs- **Large data transfer**: Dedicated 64MB channel with zero-copy architecture



- **自动资源管理**：引用计数 + PID检测 + 心跳监控- **高吞吐**：大数据通道 ~135 MB/s（1MB块）  - High throughput: ~390 MB/s

- **异常退出恢复**：崩溃节点2-5秒内自动清理

- **数据完整性**：CRC32校验、序列号检测  - Low latency: ~2ms (1MB transfer)

- **流控机制**：背压保护，防止内存溢出

### 🎯 灵活的通信模式  - Memory efficient: MAP_NORESERVE optimization (98.5% savings when idle)

### 🌐 跨平台支持

- **发布-订阅**：主题分组、多订阅者、通配符匹配  - CRC32 integrity verification

- **Linux平台**：完整支持，MAP_NORESERVE优化

- **QNX平台**：完整支持，条件编译适配- **进程内通信**：同进程内零开销- **Lock-free design**: SPSC (Single Producer Single Consumer) queues for shared memory

- **CMake构建**：现代化构建系统，支持交叉编译

- **跨进程通信**：共享内存（本地）+ UDP（可选）- **Auto cleanup**: Automatic shared memory cleanup when last node exits

---

- **动态节点发现**：自动注册与发现- **Heartbeat monitoring**: 5-second timeout for zombie node detection

## 📋 目录

- **Selective delivery**: Only subscribers receive relevant messages

- [快速开始](#-快速开始)

- [构建系统](#️-构建系统)### 🛡️ 可靠性保证- **Simple API**: Unified Node interface for all communication needs

- [API参考](#-api参考)

- [使用示例](#-使用示例)- **自动资源管理**：引用计数 + PID检测双重清理机制- **Late-joining support**: New nodes automatically discover existing subscriptions

- [性能指标](#-性能指标)

- [最佳实践](#-最佳实践)- **异常退出恢复**：kill -9后自动清理共享内存- **Self-healing**: Subscription registry maintains consistency across nodes

- [文档导航](#-文档导航)

- **数据完整性**：CRC32校验、序列号检测

---

- **流控机制**：背压保护，防止内存溢出## Architecture

## 🚀 快速开始



### 构建库（Linux）

### 📦 易用性### Design Principles

```bash

# 方式1: 使用CMake（推荐）- **简洁API**：`publish()`、`subscribe()`、`sendLargeData()`

./build.sh

- **自动初始化**：首次使用时自动清理残留资源1. **Node-centric**: Each endpoint is a Node instance

# 方式2: 使用Makefile

make -j4- **类型安全**：C++14强类型接口2. **Topic filtering**: Messages delivered only to matching subscribers

```

- **零配置启动**：默认参数即可工作3. **Process-aware**: Automatic routing between in-process and inter-process nodes

生成文件：

- `build/librpc.so.1.0.0` - 共享库（256KB）4. **Non-blocking**: UDP transport runs in background thread

- `build/test_*` - 测试程序

---

### 发布-订阅示例

### Communication Flow

**发布者**：

```cpp## 📋 目录

#include "Node.h"

```

auto node = librpc::createNode("publisher");

- [快速开始](#-快速开始)Node1 (Process A)           Node2 (Process A)           Node3 (Process B)

// 发布消息

std::string msg = "Hello, World!";- [架构设计](#-架构设计)    |                            |                            |

node->publish("sensor", "temperature", 

              (const uint8_t*)msg.data(), msg.size());- [性能指标](#-性能指标)    | subscribe("sensor", ["temp"])  subscribe("sensor", ["temp", "pressure"])

```

- [API参考](#-api参考)    |                            |                            |

**订阅者**：

```cpp- [使用示例](#-使用示例)    |                            |                            |

#include "Node.h"

- [构建与测试](#️-构建与测试)    |------- broadcast("sensor", "temp", "25C") ------------->|

auto node = librpc::createNode("subscriber");

- [最佳实践](#-最佳实践)    |                            |                            |

// 订阅回调

node->subscribe("sensor", {"temperature"},- [常见问题](#-常见问题)    | (in-process delivery)      |                            |

    [](const std::string& group, const std::string& topic,

       const uint8_t* data, size_t size) {    |--------------------------->|                            |

        std::string msg((char*)data, size);

        std::cout << "Received: " << msg << std::endl;---    |                            |                            |

    });

    |                   (shared memory broadcast)             |

// 保持运行

while (true) {## 🚀 快速开始    |-------------------------------------------------------->|

    std::this_thread::sleep_for(std::chrono::seconds(1));

}    |                            |                            |

```

### 编译库    |                    (Node3 polls SPSC queue)             |

---

    |                            |                  receives message

## 🛠️ 构建系统

```bash```

### CMake 构建（推荐）

cd librpc

**Linux 平台**：

```bashmake -j4**Transport Selection:**

# 快速构建

./build.sh```- **Same process**: Direct callback invocation (Node1 → Node2)



# 清理重建- **Different process, same host**: Lock-free shared memory (Node1 → Node3)

./build.sh -c

生成文件：- **Different host**: UDP broadcast (cross-network communication)

# Debug模式

./build.sh -d- `lib/librpc.a` - 静态库



# 静态库- `lib/librpc.so` - 动态库## API Reference

./build.sh -s

```



**QNX 平台交叉编译**：### 发布-订阅示例### Node Interface

```bash

# 设置QNX环境

export QNX_HOST=/opt/qnx710/host/linux/x86_64

export QNX_TARGET=/opt/qnx710/target/qnx7**发布者**：```cpp



# 交叉编译```cppclass Node {

./build.sh -p qnx

```#include "Node.h"public:



详细说明请参考 [CMAKE_BUILD.md](CMAKE_BUILD.md)    using Property = std::string;



### Makefile 构建（保留）auto node = librpc::createNode("publisher");    using Callback = std::function<void(const Property& msg_group, 



```bash                                       const Property& topic, 

# 编译库

make -j4// 发布小消息（<256KB）                                       const uint8_t* payload, 



# 清理std::string msg = "Hello, World!";                                       size_t size)>;

make clean

```node->publish("sensor", "temperature", 



---              (const uint8_t*)msg.data(), msg.size());    // Broadcast message to all subscribers



## 📚 API参考```    virtual Error broadcast(const Property& msg_group, 



### Node接口                          const Property& topic, 



```cpp**订阅者**：                          const Property& payload) = 0;

class Node {

public:```cpp

    // 创建节点

    static std::shared_ptr<Node> createNode(#include "Node.h"    // Subscribe to topics

        const std::string& node_id = "",

        const Config& config = Config());    virtual Error subscribe(const Property& msg_group, 

    

    // 发布消息（<256KB）auto node = librpc::createNode("subscriber");                          const std::vector<Property>& topics, 

    virtual Error publish(

        const std::string& msg_group,                          const Callback& callback) = 0;

        const std::string& topic,

        const uint8_t* data,// 订阅回调

        size_t size) = 0;

    node->subscribe("sensor", {"temperature"},     // Unsubscribe from topics

    // 订阅消息

    virtual void subscribe(    [](const std::string& group, const std::string& topic,    virtual Error unsubscribe(const Property& msg_group, 

        const std::string& msg_group,

        const std::vector<std::string>& topics,       const uint8_t* data, size_t size) {                            const std::vector<Property>& topics) = 0;

        DataCallback callback) = 0;

            std::string msg((char*)data, size);    

    // 获取节点ID

    virtual std::string getNodeId() const = 0;        std::cout << "Received: " << msg << std::endl;    // Get large data channel (for high-frequency large data transfer)

};

```    });    virtual std::shared_ptr<LargeDataChannel> getLargeDataChannel(



### 配置选项        const std::string& channel_name) = 0;



```cpp// 保持运行};

struct Config {

    // 最大入站队列数（影响内存占用）while (true) {```

    size_t max_inbound_queues = 32;  // 默认32，范围: 8-64

        std::this_thread::sleep_for(std::chrono::seconds(1));

    // 队列容量（每队列消息数）

    size_t queue_capacity = 256;     // 默认256，范围: 64-1024}### Factory Functions

};

```

// 内存占用计算：

// Memory = max_inbound_queues × queue_capacity × MESSAGE_SIZE```cpp

// 默认: 32 × 256 × 2048 = 16.8 MB

// 最大: 64 × 1024 × 2048 = 132 MB### 大数据传输示例// Create a new node

```

std::shared_ptr<Node> createNode(const std::string& node_id = "",

---

**发送端**：                                 bool use_udp = true,

## 💡 使用示例

```cpp                                 uint16_t udp_port = 0);

### 1. 进程内通信

// 准备大数据（1MB）

```cpp

#include "Node.h"std::vector<uint8_t> large_data(1024 * 1024);// Get default singleton node



// 创建两个节点// ... 填充数据 ...std::shared_ptr<Node> communicationInterface();

auto node1 = librpc::createNode("node1");

auto node2 = librpc::createNode("node2");```



// node2订阅// 发送大数据（自动使用零拷贝通道）

node2->subscribe("sensor", {"temperature"},

    [](const auto& group, const auto& topic, const auto* data, size_t size) {auto err = node->sendLargeData(## Usage Examples

        std::cout << "Node2 received: " 

                  << std::string((char*)data, size) << std::endl;    "vision",              // 消息组

    });

    "camera_channel",      // 通道名### Example 1: Basic Subscribe and Broadcast

// node1发布（node2会直接接收，零开销）

node1->publish("sensor", "temperature", "25.5C");    "image_data",         // 主题

```

    large_data.data(),    // 数据```cpp

### 2. 跨进程通信

    large_data.size()     // 大小#include "Node.h"

**进程A**：

```cpp);

auto nodeA = librpc::createNode("process_A");

```// Create node

nodeA->subscribe("ipc", {"commands"},

    [](const auto& group, const auto& topic, const auto* data, size_t size) {auto node = librpc::createNode("sensor_node");

        std::cout << "Received command: " 

                  << std::string((char*)data, size) << std::endl;**接收端**：

    });

```cpp// Subscribe to temperature topic

// 保持运行

while (true) std::this_thread::sleep_for(std::chrono::seconds(1));// 订阅大数据通知node->subscribe("sensor", {"temperature"}, 

```

node->subscribe("vision", {"image_data"},    [](const auto& group, const auto& topic, const auto* payload, size_t size) {

**进程B**：

```cpp    [](const std::string& group, const std::string& topic,        std::cout << "Temperature: " 

auto nodeB = librpc::createNode("process_B");

       const uint8_t* data, size_t size) {                  << std::string((const char*)payload, size) << std::endl;

// 发送命令（通过共享内存传输到进程A）

nodeB->publish("ipc", "commands", "START");        // 接收到通知消息    });

```

        

### 3. 大数据传输

        // 读取大数据（零拷贝）// Broadcast temperature data

```cpp

// 发送端        auto channel = librpc::LargeDataChannel::create("camera_channel");node->publish("sensor", "temperature", "25.5C");

std::vector<uint8_t> large_data(1024 * 1024);  // 1MB

// ... 填充数据 ...        librpc::LargeDataChannel::DataBlock block;```



node->sendLargeData(        

    "vision",           // 消息组

    "camera_channel",   // 通道名        if (channel->tryRead(block)) {### Example 2: Multiple Topics

    "image_data",       // 主题

    large_data.data(),            // 处理数据：block.data, block.header.size

    large_data.size()

);            // ...```cpp



// 接收端            auto node = librpc::createNode("multi_sensor");

node->subscribe("vision", {"image_data"},

    [](const auto& group, const auto& topic, const auto* data, size_t size) {            // 释放块

        // 接收到通知，读取大数据

        auto channel = librpc::LargeDataChannel::create("camera_channel");            channel->releaseBlock(block);// Subscribe to multiple topics

        librpc::LargeDataChannel::DataBlock block;

                }node->subscribe("sensor", {"temperature", "pressure", "humidity"}, 

        if (channel->tryRead(block)) {

            // 零拷贝访问：block.data 直接指向共享内存    });    [](const auto& group, const auto& topic, const auto* payload, size_t size) {

            processImage(block.data, block.header.size);

            channel->releaseBlock(block);```        std::cout << topic << ": " 

        }

    });                  << std::string((const char*)payload, size) << std::endl;

```

---    });

---

```

## 📊 性能指标

## 🏗️ 架构设计

### 延迟测试（P50/P99）

### Example 3: Multiple Nodes in Same Process

| 传输类型 | 消息大小 | P50延迟 | P99延迟 |

|---------|---------|---------|---------|### 整体架构

| 进程内通信 | 256B | <1μs | <2μs |

| 共享内存V3 | 256B | 8μs | 15μs |```cpp

| 大数据通道 | 1MB | 35μs | 80μs |

| 大数据通道 | 4MB | 120μs | 250μs |```// Node 1: Temperature publisher



### 吞吐量测试┌─────────────────────────────────────────────────────────┐auto temp_node = librpc::createNode("temp_node");



| 传输类型 | 消息大小 | 吞吐量 | QPS |│                    Application Layer                     │

|---------|---------|--------|-----|

| 共享内存V3 | 256B | ~50 MB/s | ~200,000 |│  publish() | subscribe() | sendLargeData()              │// Node 2: Temperature subscriber

| 大数据通道 | 1MB | ~135 MB/s | ~135 |

| 大数据通道 | 4MB | ~110 MB/s | ~27 |└─────────────────────────────────────────────────────────┘auto display_node = librpc::createNode("display_node");



### 内存占用（优化后）                           │display_node->subscribe("sensor", {"temperature"}, 



| 配置 | 队列数 | 容量 | 内存占用 | 场景 |┌─────────────────────────┴─────────────────────────────┐    [](const auto& group, const auto& topic, const auto* data, size_t size) {

|-----|--------|------|---------|------|

| **最小** | 8 | 64 | ~1 MB | 资源受限 |│                    Node Interface                      │        // Handle temperature

| **默认** | 32 | 256 | **33 MB** | **推荐** |

| **标准** | 64 | 256 | 33 MB | 高并发 |│  • Topic routing      • Callback management            │    });

| **最大** | 64 | 1024 | 132 MB | 高吞吐 |

│  • Node discovery     • Transport selection            │

---

└─────────────────────────┬─────────────────────────────┘// Publish temperature (display_node will receive in-process)

## 🎯 最佳实践

                           │temp_node->publish("sensor", "temperature", "26.0C");

### 1. 选择合适的配置

        ┌──────────────────┼──────────────────┐```

```cpp

// 低内存场景（嵌入式设备）        │                  │                  │

librpc::Config config;

config.max_inbound_queues = 8;   // 8MB内存┌───────▼──────┐  ┌────────▼────────┐  ┌─────▼──────────┐### Example 4: Inter-Process Communication

config.queue_capacity = 64;

auto node = librpc::createNode("low_mem", config);│ InProcess    │  │ SharedMemory V3 │  │ LargeDataChannel│



// 高并发场景（服务器）│ Transport    │  │ Transport       │  │ (Zero-copy)     │Process A:

librpc::Config config;

config.max_inbound_queues = 64;  // 33MB内存└──────────────┘  └─────────────────┘  └─────────────────┘```cpp

config.queue_capacity = 256;

auto node = librpc::createNode("high_perf", config);                          │// Create node with specific UDP port

```

                  ┌───────┴────────┐auto nodeA = librpc::createNode("nodeA", true, 47121);

### 2. 错误处理

                  │                │nodeA->subscribe("ipc", {"commands"}, 

```cpp

// ✅ 检查返回值          ┌───────▼──────┐  ┌──────▼──────┐    [](const auto& group, const auto& topic, const auto* data, size_t size) {

auto err = node->sendLargeData(...);

if (err == librpc::TIMEOUT) {          │   Registry   │  │  Node SHM   │        // Handle commands from other processes

    std::cerr << "Queue full, retry later" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(10));          │  (Discovery) │  │  (RX Queue) │    });

    // 重试...

} else if (err != librpc::NO_ERROR) {          └──────────────┘  └─────────────┘```

    std::cerr << "Send failed: " << err << std::endl;

}```

```

Process B:

### 3. 资源管理

### 三种传输模式```cpp

```cpp

// ✅ 使用RAII自动管理// Create node with different UDP port

{

    auto node = librpc::createNode("mynode");#### 1. InProcess Transport（进程内）auto nodeB = librpc::createNode("nodeB", true, 47122);

    auto channel = librpc::LargeDataChannel::create("mychannel");

    - **场景**：同进程内的节点通信// Broadcast will be received by Process A via UDP

    // 使用...

} // 自动清理- **机制**：直接函数调用，零开销nodeB->publish("ipc", "commands", "START");



// ❌ 避免手动管理- **延迟**：<1μs```

Node* node = new Node(...);  // 容易忘记delete

```



### 4. 选择传输方式#### 2. SharedMemory V3 Transport（跨进程）### Example 5: Selective Subscription



| 场景 | 数据大小 | 频率 | 推荐方式 |- **场景**：小消息传输（<256KB）

|-----|---------|------|---------|

| 控制消息 | <1KB | 低频 | `publish()` |- **架构**：```cpp

| 传感器数据 | <256KB | 中频 | `publish()` |

| 图像/视频 | >1MB | 高频 | `sendLargeData()` |  - 每个节点独立的共享内存区域（`/dev/shm/librpc_node_<pid>_<hash>`）auto node1 = librpc::createNode("node1");

| 日志 | <10KB | 高频 | `publish()` |

  - 无锁SPSC队列，动态分配auto node2 = librpc::createNode("node2");

---

  - 基于Registry的节点发现

## ❓ 常见问题

- **特性**：// Node1 subscribes only to temperature

### Q1: 共享内存残留怎么办？

  - 动态扩展（无节点数量限制）node1->subscribe("sensor", {"temperature"}, callback1);

**A**: LibRPC有自动清理机制：

  - PID检测自动清理

1. **正常退出**：引用计数自动清理

2. **异常退出（kill -9）**：下次启动时PID检测自动清理  - 心跳机制检测存活// Node2 subscribes only to pressure

3. **崩溃节点**：心跳超时5秒后自动清理

- **延迟**：<10μsnode2->subscribe("sensor", {"pressure"}, callback2);

手动清理：

```bash- **吞吐**：~50 MB/s（256字节消息）

# 查看残留

ls -lh /dev/shm/ | grep librpc// Only node1 receives this



# 清理所有#### 3. LargeDataChannel（大数据）node1->publish("sensor", "temperature", "25C");

rm -f /dev/shm/librpc_*

```- **场景**：高频大数据传输（>1MB，最大8MB）



### Q2: 内存占用太大怎么办？- **架构**：// Only node2 receives this



**A**: 调整配置降低内存占用：  - 独立共享内存通道（`/dev/shm/*_channel`）node2->publish("sensor", "pressure", "1013hPa");



```cpp  - 环形缓冲区 + 变长块```

// 默认配置：33MB

librpc::Config config;  - 零拷贝读取

config.max_inbound_queues = 32;

config.queue_capacity = 256;- **特性**：### Example 6: Large Data Transfer



// 低内存配置：8MB  - 默认64MB缓冲区

config.max_inbound_queues = 16;

config.queue_capacity = 64;  - CRC32数据校验For high-frequency large data (>100KB, >10 times/sec), use the dedicated large data channel:

```

  - 自动流控（队列满时返回TIMEOUT）

### Q3: 线程安全吗？

  - PID检测自动清理```cpp

**A**: 

- ✅ `Node::publish()` - 线程安全- **延迟**：<50μs#include "Node.h"

- ✅ `Node::subscribe()` - 线程安全

- ✅ `LargeDataChannel::write()` - 单写者线程安全- **吞吐**：~135 MB/s（1MB块）

- ✅ `LargeDataChannel::tryRead()` - 单读者线程安全

- ⚠️  不支持多写者或多读者（SPSC设计）// Create nodes



### Q4: QNX平台有什么区别？### 资源管理机制auto sender = librpc::createNode("sender");



**A**: QNX平台差异：auto receiver = librpc::createNode("receiver");

- 共享内存路径：`/dev/shmem`（Linux是`/dev/shm`）

- 不支持`MAP_NORESERVE`：立即分配物理内存#### 自动清理（双重保障）

- 其他API完全兼容

// Get large data channel (auto-configured with 64MB buffer + MAP_NORESERVE)

详见 [QNX_COMPATIBILITY.md](QNX_COMPATIBILITY.md)

**1. 引用计数清理（正常退出）**auto channel = sender->getLargeDataChannel("video_stream");

---

```cpp

## 📖 文档导航

struct RingBufferControl {// Receiver: Subscribe to notifications

| 文档 | 内容 | 适用对象 |

|-----|------|---------|    std::atomic<int32_t> ref_count;  // 引用计数receiver->subscribe("large_data", {"data_ready"}, 

| **[README.md](README.md)** (本文) | 快速开始、API参考、示例代码 | 新用户、应用开发者 |

| **[ARCHITECTURE.md](ARCHITECTURE.md)** | 详细架构设计、性能优化、设计权衡 | 高级开发者、架构师 |};    [channel](const auto& group, const auto& topic, const auto* data, size_t size) {

| **[CMAKE_BUILD.md](CMAKE_BUILD.md)** | CMake构建系统、交叉编译、IDE集成 | 构建工程师 |

| **[QNX_COMPATIBILITY.md](QNX_COMPATIBILITY.md)** | QNX平台适配、条件编译、差异说明 | QNX开发者 |        auto notif = reinterpret_cast<const librpc::LargeDataNotification*>(data);



**推荐阅读顺序**：~LargeDataChannel() {        

1. README.md（快速开始）

2. CMAKE_BUILD.md（构建系统）    int32_t prev = ref_count.fetch_sub(1);        // Read large data (zero-copy)

3. ARCHITECTURE.md（深入理解）

4. QNX_COMPATIBILITY.md（平台适配）    if (prev == 1) {  // 最后一个引用        librpc::DataBlock block;



---        shm_unlink(shm_name_);  // ✓ 自动删除        if (channel->tryRead(block, notif->sequence)) {



## 🧪 测试    }            // Process data directly (no copy)



### 运行测试}            processFrame(block.data, block.size);



```bash```            channel->releaseBlock(block);

# 使用便捷脚本

./run_tests.sh        }



# 或单独运行**2. PID检测清理（异常退出）**    });

cd build

./test_inprocess            # 进程内通信测试```cpp

./test_duplex_v2            # 双工通信测试

./test_heartbeat_timeout    # 心跳超时测试struct RingBufferControl {// Sender: Send large data

./test_service_discovery    # 服务发现测试

```    std::atomic<int32_t> writer_pid;  // 写端PIDstd::vector<uint8_t> frame(1024 * 1024);  // 1MB frame



### 测试覆盖    std::atomic<int32_t> reader_pid;  // 读端PIDint64_t seq = channel->write("frame_001", frame.data(), frame.size());



- ✅ 进程内通信（零开销）};

- ✅ 跨进程共享内存通信

- ✅ 心跳超时检测// Notify receiver via small message

- ✅ 服务发现机制

- ✅ 节点事件通知// 启动时自动清理librpc::LargeDataNotification notif{};

- ✅ 资源自动清理

LargeDataChannel::create(...) {notif.sequence = seq;

---

    static std::atomic<bool> first_time{false};notif.size = frame.size();

## 📄 许可证

    if (!first_time.exchange(true)) {sender->publish("large_data", "data_ready", 

MIT License

        cleanupOrphanedChannels();  // PID检测清理                 reinterpret_cast<const uint8_t*>(&notif), sizeof(notif));

---

    }```

## 📈 版本历史

}

### v3.0 (当前) - 内存优化 + 跨平台

- ✅ 内存占用优化（529MB → 33MB，降低94%）**Performance**: ~390 MB/s throughput, ~2ms latency  

- ✅ 配置化队列参数

- ✅ QNX平台完整支持// PID检测逻辑**Docs**: See [HIGH_FREQUENCY_LARGE_DATA_SOLUTION.md](HIGH_FREQUENCY_LARGE_DATA_SOLUTION.md)

- ✅ CMake现代化构建系统

- ✅ 心跳机制集成NODE_LEFT事件bool isProcessAlive(pid_t pid) {

- ✅ 条件编译平台适配

    return (kill(pid, 0) == 0) || (errno != ESRCH);## Build Instructions

### v2.0 - 动态共享内存

- ✅ SharedMemoryTransportV3（动态分配）}

- ✅ 无节点数量限制

- ✅ PID检测 + 引用计数双重清理```### Prerequisites

- ✅ 心跳监控机制



### v1.0 - 初始版本

- ✅ 基础发布-订阅**清理对比**：- C++14 or later

- ✅ 进程内/跨进程通信

- ✅ 大数据传输通道- Linux/QNX



---| 退出方式 | 引用计数 | PID检测 | 结果 |- pthread



**最后更新**: 2025-11-26  |---------|---------|---------|------|- socket support

**版本**: 3.0

| 正常退出 | ✅ ref→0 | ✅ PID消失 | 即时清理 |

| kill -9  | ❌ 未执行 | ✅ PID消失 | 下次启动清理 |### Compile

| 崩溃     | ❌ 未执行 | ✅ PID消失 | 下次启动清理 |

```bash

---cd librpc

make

## 📊 性能指标```



### 延迟测试（P50/P99）### Run Tests



| 传输类型 | 消息大小 | P50延迟 | P99延迟 |LibRpc uses **SharedMemoryTransportV2** with lock-free SPSC queues for high-performance inter-process communication.

|---------|---------|---------|---------|

| InProcess | 256B | <1μs | <2μs |#### Quick Test (Recommended)

| SharedMemory V3 | 256B | 8μs | 15μs |Run the complete test suite:

| LargeDataChannel | 1MB | 35μs | 80μs |```bash

| LargeDataChannel | 4MB | 120μs | 250μs |make run-tests

# Or directly:

### 吞吐量测试./run_tests.sh

```

| 传输类型 | 消息大小 | 吞吐量 | QPS |

|---------|---------|--------|-----|**Test Coverage:**

| SharedMemory V3 | 256B | ~50 MB/s | ~200,000 |1. **In-process tests**: Basic operations + 20,000 message stress test

| LargeDataChannel | 1MB | ~135 MB/s | ~135 |2. **Inter-process tests**: Sender/receiver performance validation

| LargeDataChannel | 4MB | ~110 MB/s | ~27 |3. **Cleanup tests**: Automatic shared memory cleanup verification



### 资源占用#### Individual Tests



| 组件 | 内存占用 | 说明 |**1. In-Process Communication Test**

|-----|---------|------|Tests multiple nodes within the same process using lock-free shared memory:

| Registry | 4MB | 节点注册表（共享） |```bash

| Node SHM | 528MB | 每节点接收队列 |LD_LIBRARY_PATH=./lib ./test_inprocess basic

| LargeDataChannel | 64MB | 每通道（可配置） |LD_LIBRARY_PATH=./lib ./test_inprocess stress

LD_LIBRARY_PATH=./lib ./test_inprocess all

---```



## 📚 API参考**What This Tests:**

- Node registration in shared memory (max 8 nodes)

### Node接口- In-process message delivery via SPSC queues

- Selective subscription (only matching subscribers receive messages)

```cpp- Stress test: 20,000 messages, ~493,000 msg/s throughput

class Node {- No message duplication, 100% delivery rate

public:

    // 创建节点**2. Inter-Process Communication Test**

    static std::shared_ptr<Node> createNode(const std::string& node_id);Tests lock-free shared memory communication across processes:

    

    // 发布小消息（<256KB）Terminal 1 (Receiver):

    virtual Error publish(```bash

        const std::string& msg_group,LD_LIBRARY_PATH=./lib ./test_interprocess_receiver

        const std::string& topic,```

        const uint8_t* data,

        size_t size) = 0;Terminal 2 (Sender - start after receiver is ready):

    ```bash

    // 订阅消息LD_LIBRARY_PATH=./lib ./test_interprocess_sender

    virtual void subscribe(```

        const std::string& msg_group,

        const std::vector<std::string>& topics,**What This Tests:**

        DataCallback callback) = 0;- Cross-process SPSC queue communication

    - Lock-free concurrent access (no mutex contention)

    // 发送大数据（>1MB，最大8MB）- Sender performance: ~1,362,000 msg/s

    virtual Error sendLargeData(- Receiver performance: ~979 msg/s

        const std::string& msg_group,- Shared memory: /dev/shm/librpc_shm_v2 (132MB)

        const std::string& channel_name,

        const std::string& topic,**3. Automatic Cleanup Test**

        const uint8_t* data,Tests shared memory lifecycle management:

        size_t size) = 0;```bash

    LD_LIBRARY_PATH=./lib ./test_cleanup

    // 获取节点ID```

    virtual std::string getNodeId() const = 0;

};**What This Tests:**

```- Last node triggers shm_unlink

- Heartbeat-based zombie node detection (5s timeout)

### LargeDataChannel接口- Orphaned memory cleanup

- Multiple create/destroy cycles

```cpp- 6 test scenarios, all PASSED

class LargeDataChannel {

public:**4. Large Data Transfer Test**

    // 创建/连接通道Tests high-performance large data channel:

    static std::shared_ptr<LargeDataChannel> create(```bash

        const std::string& shm_name,./run_large_data_test.sh

        const Config& config = Config());# Or manually:

    make test-large

    // 写入数据（发送端）LD_LIBRARY_PATH=./lib ./test_large_receiver &

    int64_t write(const std::string& topic,LD_LIBRARY_PATH=./lib ./test_large_sender 50 1024  # 50×1MB

                  const uint8_t* data,```

                  size_t size);

    **What This Tests:**

    // 尝试读取（接收端，零拷贝）- 64MB ring buffer with MAP_NORESERVE optimization

    bool tryRead(DataBlock& block);- Zero-copy read/write operations

    - CRC32 data integrity verification

    // 释放数据块- 5 performance scenarios (512KB to 4MB blocks)

    void releaseBlock(const DataBlock& block);- Average throughput: ~390 MB/s

    - 100% data integrity, zero CRC errors

    // 清理孤儿通道（静态工具）

    static size_t cleanupOrphanedChannels(uint32_t timeout_seconds = 60);## Message Protocol

};

```### Packet Structure



---```

+--------+--------+----------+----------+----------+----------+

## 💡 使用示例| Magic  | Version| GroupLen | TopicLen | PayloadLen| Checksum|

| 4bytes | 2bytes | 2bytes   | 2bytes   | 4bytes   | 4bytes  |

完整示例请参考 `test_*.cpp` 文件。+--------+--------+----------+----------+----------+----------+

| NodeID (64 bytes)                                           |

### 编译示例+-------------------------------------------------------------+

| Group Data (variable)                                       |

```bash+-------------------------------------------------------------+

# 编译库| Topic Data (variable)                                       |

make -j4+-------------------------------------------------------------+

| Payload Data (variable)                                     |

# 编译示例程序+-------------------------------------------------------------+

g++ -std=c++14 -Iinclude -pthread my_app.cpp -o my_app -Llib -lrpc -lrt```



# 运行### UDP Configuration

LD_LIBRARY_PATH=./lib ./my_app

```- **Port assignment**: Each node binds to a unique UDP port

- **Node discovery**: Port scanning (localhost ports 47200-47230, 48000-48020)

---- **Communication**: Point-to-point UDP based on discovered node addresses

- **Max message size**: ~64KB

## 🛠️ 构建与测试- **Protocol**: Custom message format with checksums



### 构建## Performance Characteristics



```bash### In-Process Communication

# 编译库

make -j4- **Latency**: < 1μs (direct function call)

- **Throughput**: > 1M msg/s (limited only by callback processing)

# 清理- **Memory**: Zero-copy

make clean

### Inter-Process Communication (Shared Memory V2)

# 运行测试

make test-large      # 大数据传输测试- **Latency**: ~1-2μs (lock-free SPSC queue)

```- **Throughput**: 

  - Send: ~1,000,000 msg/s (parallel write to multiple queues)

### 测试程序  - Receive: ~500,000 msg/s (polling from SPSC queues)

  - In-process: ~493,000 msg/s (full duplex)

```bash- **Memory**: 132MB shared memory (8 nodes × 1024 msg/queue)

# 1. 进程内通信测试- **Architecture**: N×N SPSC queue matrix (64 queues for 8 nodes)

./test_inprocess- **Advantages**: 

  - No mutex contention (lock-free)

# 2. V3双工通信测试（两个终端）  - Zero-copy (direct memory access)

# 终端1：  - Atomic operations only (write_pos, read_pos)

LD_LIBRARY_PATH=./lib ./test_duplex_v2 node0 node1 10 256 10000  - 89x faster than mutex-based approach



# 终端2：### Cross-Host Communication (UDP)

LD_LIBRARY_PATH=./lib ./test_duplex_v2 node1 node0 10 256 10000

- **Latency**: ~100μs (localhost), higher for network

# 3. 大数据完整性测试（两个终端）- **Throughput**: ~100K messages/sec

# 终端1（接收端）：- **Memory**: One copy (serialization)

LD_LIBRARY_PATH=./lib ./test_data_integrity receiver- **Use case**: Cross-subnet, cross-host messaging



# 终端2（发送端）：## Thread Safety

LD_LIBRARY_PATH=./lib ./test_data_integrity sender

- All public APIs are thread-safe

# 4. V3 PID清理测试- Callbacks may be invoked from different threads

LD_LIBRARY_PATH=./lib ./test_v3_pid_cleanup- Use proper synchronization in callbacks if needed

```

## Error Handling

---

```cpp

## 🎯 最佳实践enum Error {

    NO_ERROR         = 0,

### 1. 选择合适的传输方式    INVALID_ARG      = 1,

    NOT_INITIALIZED  = 2,

| 场景 | 数据大小 | 频率 | 推荐方式 |    ALREADY_EXISTS   = 3,

|-----|---------|------|---------|    NOT_FOUND        = 4,

| 控制消息 | <1KB | 低频 | `publish()` |    NETWORK_ERROR    = 5,

| 传感器数据 | <256KB | 中频 | `publish()` |    TIMEOUT          = 6,

| 图像/视频 | >1MB | 高频 | `sendLargeData()` |    UNEXPECTED_ERROR = 99,

| 日志 | <10KB | 高频 | `publish()` |};

```

### 2. 资源管理

## Limitations

```cpp

// ✅ 好的做法：使用RAII1. **Node limit**: Maximum 8 nodes per host (configurable via MAX_NODES)

{2. **Queue capacity**: 1024 messages per queue (configurable via QUEUE_CAPACITY)

    auto node = librpc::createNode("mynode");3. **Message size**: Single message limited to 2KB in shared memory (MESSAGE_SIZE)

    auto channel = librpc::LargeDataChannel::create("mychannel");4. **Shared memory**: Same-host only, does not work across network

    5. **UDP limitations**: Broadcast may not work across subnets, ~64KB message size

    // 使用...6. **Delivery guarantee**: Best-effort (queue full = drop message)

    7. **No encryption**: No built-in encryption/authentication

} // 自动清理

## Shared Memory Configuration

// ❌ 避免：手动管理

Node* node = new Node(...);Edit `include/SharedMemoryTransportV2.h` to adjust:

// ... 可能忘记delete

``````cpp

static constexpr int MAX_NODES = 8;          // Maximum nodes

### 3. 错误处理static constexpr int QUEUE_CAPACITY = 1024;  // Messages per queue

static constexpr int MESSAGE_SIZE = 2048;    // Bytes per message

```cppstatic constexpr int HEARTBEAT_INTERVAL = 1; // Seconds

// ✅ 检查返回值static constexpr int NODE_TIMEOUT = 5;       // Seconds

auto err = node->sendLargeData(...);```

if (err == librpc::TIMEOUT) {

    std::cerr << "Queue full, retry later" << std::endl;**Memory calculation**: 

    std::this_thread::sleep_for(std::chrono::milliseconds(10));```

    // 重试...Total = MAX_NODES × MAX_NODES × QUEUE_CAPACITY × MESSAGE_SIZE

} else if (err != librpc::NO_ERROR) {      = 8 × 8 × 1024 × 2048 bytes

    std::cerr << "Send failed: " << err << std::endl;      ≈ 132 MB

}```

```

## Best Practices

---

1. **Use unique node IDs** for easier debugging

## ❓ 常见问题2. **Keep callbacks fast** to avoid blocking receive thread

3. **Subscribe before broadcast** in same-process scenarios

### Q1: 共享内存残留怎么办？4. **Monitor queue capacity** - adjust QUEUE_CAPACITY if messages are dropped

5. **Limit node count** - stay within MAX_NODES (default 8)

**A**: LibRPC有自动清理机制：6. **Handle large messages** - use UDP for messages > 2KB

7. **Check shared memory** - use `ls -lh /dev/shm/librpc_shm_v2` to monitor

1. **正常退出**：引用计数自动清理8. **Clean shutdown** - let nodes exit gracefully for automatic cleanup

2. **异常退出（kill -9）**：下次启动时PID检测自动清理9. **Handle callback exceptions** to prevent crashes



手动清理：## Troubleshooting

```bash

# 查看残留### Shared Memory Issues

ls -lh /dev/shm/ | grep librpc

**Problem**: "Failed to create shared memory"

# 清理所有```bash

rm -f /dev/shm/librpc_*# Check if memory already exists

ls -lh /dev/shm/librpc_shm_v2

# 或使用测试程序

LD_LIBRARY_PATH=./lib ./test_v3_pid_cleanup# Manual cleanup (if nodes didn't exit gracefully)

```rm /dev/shm/librpc_shm_v2



### Q2: 发送大数据返回TIMEOUT？# Or use cleanup utility

LD_LIBRARY_PATH=./lib ./test_cleanup

**A**: 队列满了，接收端处理太慢：```



```cpp**Problem**: Messages being dropped

// 解决方案1：增大缓冲区- Increase `QUEUE_CAPACITY` in `SharedMemoryTransportV2.h`

LargeDataChannel::Config config;- Recompile: `make clean && make`

config.buffer_size = 128 * 1024 * 1024;  // 128MB- Trade-off: Higher capacity = more memory



// 解决方案2：重试机制**Problem**: "Too many nodes"

auto err = node->sendLargeData(...);- Maximum is `MAX_NODES` (default 8)

while (err == librpc::TIMEOUT) {- Increase in `SharedMemoryTransportV2.h` if needed

    std::this_thread::sleep_for(std::chrono::milliseconds(10));- Note: Memory usage grows as N²

    err = node->sendLargeData(...);

}### Performance Tuning

```

**Maximize throughput**:

### Q3: 线程安全吗？```cpp

// In SharedMemoryTransportV2.h

**A**: static constexpr int QUEUE_CAPACITY = 2048;  // Double capacity

- ✅ `Node::publish()` - 线程安全```

- ✅ `Node::subscribe()` - 线程安全

- ✅ `LargeDataChannel::write()` - 单写者**Minimize memory**:

- ✅ `LargeDataChannel::tryRead()` - 单读者```cpp

- ⚠️  不支持多写者或多读者static constexpr int MAX_NODES = 4;          // Fewer nodes

static constexpr int QUEUE_CAPACITY = 512;   // Smaller queues

---// Memory: 4×4×512×2048 = 16MB

```

## 📖 延伸阅读

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) - 详细架构设计

- [SHARED_MEMORY_AUTO_CLEANUP.md](SHARED_MEMORY_AUTO_CLEANUP.md) - 自动清理机制

- [LARGE_DATA_TRANSFER_GUIDE.md](LARGE_DATA_TRANSFER_GUIDE.md) - 大数据传输指南

- **TEST_README.md**: Comprehensive test suite documentation

---- **.backup/**: Legacy SharedMemoryTransport (mutex-based) for reference



## 📄 许可证## License



MIT LicenseCopyright (c) 2025 Baidu.com, Inc. All Rights Reserved



------



**最后更新**: 2025-11-24## Version History


### v2.0.0 (Current) - Lock-Free Shared Memory
- ✅ SharedMemoryTransportV2 with SPSC queues (no mutex)
- ✅ 89x performance improvement over mutex-based approach
- ✅ Automatic shm_unlink cleanup when last node exits
- ✅ Heartbeat monitoring with 5-second timeout
- ✅ Memory optimization: 17GB → 132MB (99.2% reduction)
- ✅ Fixed in-process message duplication bug
- ✅ Added `getLocalNodes()` and `isLocalNode()` APIs
- ✅ Comprehensive test suite (in-process, inter-process, cleanup)

### v1.1.0 - Bug Fixes
- Fixed self-subscription bug
- Fixed port scanning blind spots
- Fixed in-process UDP message duplication
- Optimized local node detection (80% faster)

### v1.0.0 - Initial Release
- Basic in-process/inter-process communication
- Subscribe/publish mechanism
- UDP transport

## Quick Start

```bash
# 1. Build
make clean && make

# 2. Run tests
make run-tests

# 3. Integrate into your project
#include "Node.h"
auto node = librpc::createNode("my_node");
node->subscribe("group", {"topic"}, callback);
node->publish("group", "topic", "data");
```

**Key Features**: Lock-free, high-performance (500K msg/s), auto-cleanup, 132MB footprint.

---

## 📖 文档导航

| 文档 | 内容 | 适用对象 |
|-----|------|---------|
| **README.md** (本文) | 快速开始、API参考、示例代码 | 新用户、应用开发者 |
| **[ARCHITECTURE.md](ARCHITECTURE.md)** | 详细架构设计、性能优化、设计权衡 | 高级开发者、架构师 |
| **[SHARED_MEMORY_AUTO_CLEANUP.md](SHARED_MEMORY_AUTO_CLEANUP.md)** | 资源清理机制（引用计数+PID检测） | 系统集成工程师 |
| **[LARGE_DATA_TRANSFER_GUIDE.md](LARGE_DATA_TRANSFER_GUIDE.md)** | 大数据传输详细指南（零拷贝） | 摄像头/传感器开发者 |
| **[HIGH_FREQUENCY_LARGE_DATA_SOLUTION.md](HIGH_FREQUENCY_LARGE_DATA_SOLUTION.md)** | 高频大数据性能优化实践 | 性能调优工程师 |

**推荐阅读顺序**：
1. README.md（快速开始）
2. ARCHITECTURE.md（理解设计）
3. 根据需求阅读专题文档

---

**最后更新**: 2025-11-24  
**版本**: 3.0
