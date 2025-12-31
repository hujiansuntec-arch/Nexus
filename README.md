# LibRPC - 高性能进程间通信库

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++14](https://img.shields.io/badge/C%2B%2B-14-blue.svg)](https://en.cppreference.com/w/cpp/14)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20QNX-lightgrey.svg)](https://www.qnx.com/)

**LibRPC (Nexus)** - 专为车载QNX系统设计的零拷贝、无锁进程间通信库。采用动态共享内存、批量通知优化和双通知机制，实现低延迟、低CPU占用的高性能IPC。

> **最新版本 v3.0**：移除FIFO方案，优化CPU占用从8.6%降到5.7%（⬇️34%）

---

## ✨ 核心特性

### 🚀 极致性能

- **零拷贝共享内存**：动态无锁SPSC队列，CPU占用<6%
- **低延迟**：进程内 <1μs，跨进程 <10μs（P50）
- **高吞吐**：50,000 msg/s @ 256字节
- **批量通知优化**：减少60%唤醒操作

### 🎯 双通知机制

| 机制 | CPU占用 | 丢包率 | 适用场景 |
|------|---------|--------|----------|
| **Condition Variable** | **5.7%** ✅ | 0.027% | 通用Linux/QNX（推荐） |
| **Semaphore** | **5.9%** ✅ | 0% | QNX嵌入式系统 |

### 🛡️ 高可靠性

- **三重清理机制**：引用计数 + PID检测 + 心跳监控
- **异常退出恢复**：kill -9后自动清理
- **数据完整性**：CRC32校验、序列号检测
- **流控保护**：背压机制防止内存溢出

### 🌐 跨平台支持

- **Linux**：完整支持，MAP_NORESERVE优化（开发/测试）
- **QNX 7.1+**：完整支持，条件编译优化（目标平台）
- **CMake**：现代化构建系统，支持交叉编译

---

## 📋 目录

- [快速开始](#-快速开始)
- [API参考](#-api参考)
- [使用示例](#-使用示例)
- [性能指标](#-性能指标)
- [配置选项](#-配置选项)
- [构建系统](#️-构建系统)
- [最佳实践](#-最佳实践)
- [文档导航](#-文档导航)

---

## 🚀 快速开始

### 构建库（Linux）

```bash
cd librpc

# 方式1：使用构建脚本（推荐）
./build.sh

# 方式2：手动CMake
mkdir -p build && cd build
cmake ..
make -j4

# 方式3：使用旧Makefile
make -j4
```

**生成文件**：
- `build/librpc.so.1.0.0` - 共享库（~256KB）
- `build/test_*` - 测试程序

### QNX交叉编译

```bash
# 设置QNX环境
export QNX_HOST=/opt/qnx710/host/linux/x86_64
export QNX_TARGET=/opt/qnx710/target/qnx7

# 交叉编译
./build.sh -p qnx
```

详见 [CMAKE_BUILD.md](CMAKE_BUILD.md)

---

## 📚 API参考

### Node接口

```cpp
class Node {
public:
    // 创建节点
    static std::shared_ptr<Node> createNode(
        const std::string& node_id = "",
        const Config& config = Config());
    
    // 发布消息（<256KB）
    virtual Error publish(
        const std::string& msg_group,
        const std::string& topic,
        const uint8_t* data,
        size_t size) = 0;
    
    // 订阅消息
    virtual void subscribe(
        const std::string& msg_group,
        const std::vector<std::string>& topics,
        DataCallback callback) = 0;
    
    // 发送大数据（>1MB，最大8MB）
    virtual Error sendLargeData(
        const std::string& msg_group,
        const std::string& channel_name,
        const std::string& topic,
        const uint8_t* data,
        size_t size) = 0;
    
    // 获取节点ID
    virtual std::string getNodeId() const = 0;
};
```

### 配置选项

```cpp
struct Config {
    // 最大入站队列数（影响内存占用）
    size_t max_inbound_queues = 32;  // 默认32，范围: 8-64
    
    // 队列容量（每队列消息数）
    size_t queue_capacity = 256;     // 默认256，范围: 64-1024
    
    // 通知机制选择
    NotifyMechanism notify_mechanism = CONDITION_VARIABLE;  // 或 SEMAPHORE
};

// 内存占用计算：
// Memory = max_inbound_queues × queue_capacity × MESSAGE_SIZE
// 默认: 32 × 256 × 2048 = 16.8 MB (单节点RX)
//      + Registry (4MB) ≈ 33 MB
```

### 错误码

```cpp
enum Error {
    NO_ERROR = 0,           // 成功
    INVALID_ARG = 1,        // 参数无效
    NOT_INITIALIZED = 2,    // 未初始化
    NOT_FOUND = 4,          // 节点未找到
    TIMEOUT = 6,            // 超时（队列满）
    UNEXPECTED_ERROR = 99   // 未预期错误
};
```

---

## 💡 使用示例

### 1. 基本发布-订阅

**发布者**：
```cpp
#include "Node.h"

auto node = librpc::createNode("publisher");

// 发布消息
std::string msg = "Hello, World!";
node->publish("sensor", "temperature", 
              (const uint8_t*)msg.data(), msg.size());
```

**订阅者**：
```cpp
#include "Node.h"

auto node = librpc::createNode("subscriber");

// 订阅回调
node->subscribe("sensor", {"temperature"},
    [](const std::string& group, const std::string& topic,
       const uint8_t* data, size_t size) {
        std::string msg((char*)data, size);
        std::cout << "Received: " << msg << std::endl;
    });

// 保持运行
while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
}
```

### 2. 跨进程通信

**进程A**（订阅者）：
```cpp
auto nodeA = librpc::createNode("process_A");
nodeA->subscribe("ipc", {"commands"},
    [](const auto& group, const auto& topic, 
       const auto* data, size_t size) {
        std::cout << "Received command: " 
                  << std::string((char*)data, size) << std::endl;
    });

// 保持运行
while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
```

**进程B**（发布者）：
```cpp
auto nodeB = librpc::createNode("process_B");

// 发送命令（通过共享内存传输到进程A）
nodeB->publish("ipc", "commands", "START");
```

### 3. 大数据传输

**发送端**（>1MB数据）：
```cpp
// 准备大数据（1MB）
std::vector<uint8_t> large_data(1024 * 1024);
// ... 填充数据 ...

// 发送大数据（自动使用零拷贝通道）
auto err = node->sendLargeData(
    "vision",              // 消息组
    "camera_channel",      // 通道名
    "image_data",          // 主题
    large_data.data(),     // 数据
    large_data.size()      // 大小
);

if (err == librpc::TIMEOUT) {
    // 队列满，重试
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

**接收端**（零拷贝读取）：
```cpp
// 订阅大数据通知
node->subscribe("vision", {"image_data"},
    [](const std::string& group, const std::string& topic,
       const uint8_t* data, size_t size) {
        
        // 接收到通知消息
        auto notif = reinterpret_cast<const librpc::LargeDataNotification*>(data);
        
        // 读取大数据（零拷贝）
        auto channel = librpc::LargeDataChannel::create("camera_channel");
        librpc::LargeDataChannel::DataBlock block;
        
        if (channel->tryRead(block, notif->sequence)) {
            // block.data 直接指向共享内存（零拷贝）
            processImage(block.data, block.header.size);
            
            // 处理完后释放
            channel->releaseBlock(block);
        }
    });
```

---

## 📊 性能指标

### 延迟测试（P50/P99，1000 msg/s负载）

| 传输类型 | 消息大小 | P50延迟 | P99延迟 |
|---------|---------|---------|---------|
| 进程内通信 | 256B | <1μs | <2μs |
| 共享内存V3 (CV) | 256B | 8μs | 15μs |
| 共享内存V3 (Sem) | 256B | 8μs | 14μs |
| 大数据通道 | 1MB | 35μs | 80μs |

### 吞吐量测试

| 传输类型 | 消息大小 | 吞吐量 | QPS |
|---------|---------|--------|-----|
| 共享内存V3 | 256B | ~50 MB/s | ~200,000 |
| 大数据通道 | 1MB | ~135 MB/s | ~135 |
| 大数据通道 | 4MB | ~110 MB/s | ~27 |

### CPU占用（1000 msg/s）

| 通知机制 | node0 CPU | node1 CPU | 平均CPU | 丢包率 |
|---------|-----------|-----------|---------|--------|
| **Condition Variable** | 5.4% | 6.0% | **5.7%** ✅ | 0.027% |
| **Semaphore** | 5.8% | 6.0% | **5.9%** ✅ | 0% |

**优化效果**：
- CPU从优化前8.6%降到5.7%（⬇️**34%**）
- 减少60%的唤醒操作
- 丢包率<0.1%

### 内存占用

| 配置 | 队列数 | 容量 | 内存占用 | 场景 |
|-----|--------|------|---------|------|
| **最小** | 8 | 64 | ~1 MB | 资源受限 |
| **默认** | 32 | 256 | **33 MB** | **推荐** ✅ |
| **标准** | 64 | 256 | 33 MB | 高并发 |
| **最大** | 64 | 1024 | 132 MB | 高吞吐 |

---

## ⚙️ 配置选项

### 选择合适的配置

```cpp
// 低内存场景（嵌入式设备）
librpc::Config config;
config.max_inbound_queues = 8;   // ~5MB内存
config.queue_capacity = 64;
config.notify_mechanism = librpc::NotifyMechanism::SEMAPHORE;
auto node = librpc::createNode("low_mem", config);

// 高并发场景（服务器）
librpc::Config config;
config.max_inbound_queues = 64;  // ~40MB内存
config.queue_capacity = 256;
config.notify_mechanism = librpc::NotifyMechanism::CONDITION_VARIABLE;
auto node = librpc::createNode("high_perf", config);
```

### 选择通知机制

```cpp
// 推荐：Condition Variable（通用）
config.notify_mechanism = librpc::NotifyMechanism::CONDITION_VARIABLE;

// 可选：Semaphore（QNX优化）
config.notify_mechanism = librpc::NotifyMechanism::SEMAPHORE;
```

---

## 🛠️ 构建系统

### CMake构建（推荐）

**Linux平台**：
```bash
# 快速构建
./build.sh

# 清理重建
./build.sh -c

# Debug模式
./build.sh -d

# 静态库
./build.sh -s
```

**QNX平台交叉编译**：
```bash
# 设置QNX环境
export QNX_HOST=/opt/qnx710/host/linux/x86_64
export QNX_TARGET=/opt/qnx710/target/qnx7

# 交叉编译
./build.sh -p qnx
```

详细说明请参考 [CMAKE_BUILD.md](CMAKE_BUILD.md)

### 测试程序

```bash
# 运行所有测试
./run_tests.sh

# 或单独运行
cd build
./test_inprocess            # 进程内通信测试
./test_duplex_v2 node0 node1 10 256 10000  # 双工通信测试
```

---

## 🎯 最佳实践

### 1. 选择传输方式

| 场景 | 数据大小 | 频率 | 推荐方式 |
|-----|---------|------|---------|
| 控制消息 | <1KB | 低频 | `publish()` |
| 传感器数据 | <256KB | 中频 | `publish()` |
| 图像/视频 | >1MB | 高频 | `sendLargeData()` |
| 日志 | <10KB | 高频 | `publish()` |

### 2. 错误处理

```cpp
// ✅ 检查返回值并重试
auto err = node->sendLargeData(...);
if (err == librpc::TIMEOUT) {
    std::cerr << "Queue full, retry later" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // 重试...
} else if (err != librpc::NO_ERROR) {
    std::cerr << "Send failed: " << err << std::endl;
}
```

### 3. 资源管理

```cpp
// ✅ 使用RAII自动管理
{
    auto node = librpc::createNode("mynode");
    auto channel = librpc::LargeDataChannel::create("mychannel");
    
    // 使用...
} // 自动清理

// ❌ 避免手动管理
Node* node = new Node(...);  // 容易忘记delete
```

### 4. 线程安全

- ✅ `Node::publish()` - 线程安全
- ✅ `Node::subscribe()` - 线程安全
- ✅ `LargeDataChannel::write()` - 单写者线程安全
- ✅ `LargeDataChannel::tryRead()` - 单读者线程安全
- ⚠️  不支持多写者或多读者（SPSC设计）

---

## ❓ 常见问题

### Q1: 共享内存残留怎么办？

**A**: LibRPC有自动清理机制：

1. **正常退出**：引用计数自动清理
2. **异常退出（kill -9）**：下次启动时PID检测自动清理
3. **崩溃节点**：心跳超时5秒后自动清理

**手动清理**：
```bash
# 查看残留
ls -lh /dev/shm/ | grep librpc

# 清理所有
rm -f /dev/shm/librpc_*

# 或使用测试程序
cd build
./test_v3_pid_cleanup
```

### Q2: 发送大数据返回TIMEOUT？

**A**: 队列满了，接收端处理太慢：

```cpp
// 解决方案1：增大缓冲区
LargeDataChannel::Config config;
config.buffer_size = 128 * 1024 * 1024;  // 128MB

// 解决方案2：重试机制
auto err = node->sendLargeData(...);
while (err == librpc::TIMEOUT) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    err = node->sendLargeData(...);
}
```

### Q3: 内存占用太大怎么办？

**A**: 调整配置降低内存占用：

```cpp
// 默认配置：40MB
librpc::Config config;
config.max_inbound_queues = 64;
config.queue_capacity = 256;

// 低内存配置：10MB
config.max_inbound_queues = 16;
config.queue_capacity = 64;
```

### Q4: QNX平台有什么区别？

**A**: QNX平台差异：

- 共享内存路径：`/dev/shmem`（Linux是`/dev/shm`）
- 不支持`MAP_NORESERVE`：立即分配物理内存
- 其他API完全兼容

详见 [QNX_COMPATIBILITY.md](QNX_COMPATIBILITY.md)

---

## 📖 文档导航

| 文档 | 内容 | 适用对象 |
|------|------|----------|
| **[README.md](README.md)** (本文) | 快速开始、API参考、示例代码 | 新用户、应用开发者 |
| **[DESIGN_DOC.md](DESIGN_DOC.md)** | 详细设计文档、核心组件、优化策略 | 高级开发者 |
| **[ARCHITECTURE.md](ARCHITECTURE.md)** | 架构设计、性能优化、设计权衡 | 架构师 |
| **[CMAKE_BUILD.md](CMAKE_BUILD.md)** | CMake构建系统、交叉编译、IDE集成 | 构建工程师 |
| **[QNX_COMPATIBILITY.md](QNX_COMPATIBILITY.md)** | QNX平台适配、条件编译、差异说明 | QNX开发者 |

**推荐阅读顺序**：
1. README.md（快速入门）
2. DESIGN_DOC.md（理解设计）
3. ARCHITECTURE.md（深入架构）
4. QNX_COMPATIBILITY.md（平台适配）

---

## 🧪 测试

### 运行测试

```bash
# 使用便捷脚本
./run_tests.sh

# 或单独运行
cd build
./test_inprocess            # 进程内通信测试
./test_duplex_v2            # 双工通信测试
./test_heartbeat_timeout    # 心跳超时测试
./test_service_discovery    # 服务发现测试
```

### 测试覆盖

- ✅ 进程内通信（零开销）
- ✅ 跨进程共享内存通信
- ✅ 心跳超时检测
- ✅ 服务发现机制
- ✅ 节点事件通知
- ✅ 资源自动清理

---

## 📄 许可证

MIT License

---

## 📈 版本历史

### v3.0 (2025-11-28) - 性能优化 + 简化架构

- ✅ **移除FIFO方案**：简化为CV + Semaphore双机制
- ✅ **批量通知优化**：CPU从8.6%降到5.7%（⬇️34%）
- ✅ **缓存活跃队列**：减少90%遍历开销
- ✅ **自适应超时**：空闲50ms、繁忙5ms动态切换
- ✅ **Flags安全验证**：双重保护机制
- ✅ **详细设计文档**：新增DESIGN_DOC.md

### v3.0 (2025-11-26) - 内存优化 + 跨平台

- ✅ 内存占用优化（529MB → 40MB，降低92%）
- ✅ 配置化队列参数
- ✅ QNX平台完整支持
- ✅ CMake现代化构建系统
- ✅ 心跳机制集成NODE_LEFT事件
- ✅ 条件编译平台适配

### v2.0 - 动态共享内存

- ✅ SharedMemoryTransportV3（动态分配）
- ✅ 无节点数量限制
- ✅ PID检测 + 引用计数双重清理
- ✅ 心跳监控机制

### v1.0 - 初始版本

- ✅ 基础发布-订阅
- ✅ 进程内/跨进程通信
- ✅ 大数据传输通道

---

**最后更新**: 2025-11-28  
**版本**: 3.0  
**维护者**: LibRPC开发团队
