# LibRpc Communication Framework

A lightweight, high-performance peer-to-peer RPC communication framework supporting in-process, inter-process (lock-free shared memory), and cross-host (UDP) communication.

## Features

- **Peer-to-peer architecture**: All endpoints are equal nodes
- **Topic-based pub/sub**: Subscribe to specific topics within message groups
- **Multi-node support**: Multiple nodes can coexist in the same process (up to 8 nodes)
- **Triple transport mechanism**: 
  - In-process: Direct function calls (zero-copy, < 1μs latency)
  - Inter-process: Lock-free shared memory SPSC queues (~500K msg/s)
  - Cross-host: UDP broadcast/point-to-point
- **Lock-free design**: SPSC (Single Producer Single Consumer) queues for shared memory
- **Auto cleanup**: Automatic shared memory cleanup when last node exits
- **Heartbeat monitoring**: 5-second timeout for zombie node detection
- **Selective delivery**: Only subscribers receive relevant messages
- **Simple API**: 3 main operations (subscribe, unsubscribe, broadcast)
- **Late-joining support**: New nodes automatically discover existing subscriptions
- **Self-healing**: Subscription registry maintains consistency across nodes

## Architecture

### Design Principles

1. **Node-centric**: Each endpoint is a Node instance
2. **Topic filtering**: Messages delivered only to matching subscribers
3. **Process-aware**: Automatic routing between in-process and inter-process nodes
4. **Non-blocking**: UDP transport runs in background thread

### Communication Flow

```
Node1 (Process A)           Node2 (Process A)           Node3 (Process B)
    |                            |                            |
    | subscribe("sensor", ["temp"])  subscribe("sensor", ["temp", "pressure"])
    |                            |                            |
    |                            |                            |
    |------- broadcast("sensor", "temp", "25C") ------------->|
    |                            |                            |
    | (in-process delivery)      |                            |
    |--------------------------->|                            |
    |                            |                            |
    |                   (shared memory broadcast)             |
    |-------------------------------------------------------->|
    |                            |                            |
    |                    (Node3 polls SPSC queue)             |
    |                            |                  receives message
```

**Transport Selection:**
- **Same process**: Direct callback invocation (Node1 → Node2)
- **Different process, same host**: Lock-free shared memory (Node1 → Node3)
- **Different host**: UDP broadcast (cross-network communication)

## API Reference

### Node Interface

```cpp
class Node {
public:
    using Property = std::string;
    using Callback = std::function<void(const Property& msg_group, 
                                       const Property& topic, 
                                       const uint8_t* payload, 
                                       size_t size)>;

    // Broadcast message to all subscribers
    virtual Error broadcast(const Property& msg_group, 
                          const Property& topic, 
                          const Property& payload) = 0;

    // Subscribe to topics
    virtual Error subscribe(const Property& msg_group, 
                          const std::vector<Property>& topics, 
                          const Callback& callback) = 0;

    // Unsubscribe from topics
    virtual Error unsubscribe(const Property& msg_group, 
                            const std::vector<Property>& topics) = 0;
};
```

### Factory Functions

```cpp
// Create a new node
std::shared_ptr<Node> createNode(const std::string& node_id = "",
                                 bool use_udp = true,
                                 uint16_t udp_port = 0);

// Get default singleton node
std::shared_ptr<Node> communicationInterface();
```

## Usage Examples

### Example 1: Basic Subscribe and Broadcast

```cpp
#include "Node.h"

// Create node
auto node = librpc::createNode("sensor_node");

// Subscribe to temperature topic
node->subscribe("sensor", {"temperature"}, 
    [](const auto& group, const auto& topic, const auto* payload, size_t size) {
        std::cout << "Temperature: " 
                  << std::string((const char*)payload, size) << std::endl;
    });

// Broadcast temperature data
node->broadcast("sensor", "temperature", "25.5C");
```

### Example 2: Multiple Topics

```cpp
auto node = librpc::createNode("multi_sensor");

// Subscribe to multiple topics
node->subscribe("sensor", {"temperature", "pressure", "humidity"}, 
    [](const auto& group, const auto& topic, const auto* payload, size_t size) {
        std::cout << topic << ": " 
                  << std::string((const char*)payload, size) << std::endl;
    });
```

### Example 3: Multiple Nodes in Same Process

```cpp
// Node 1: Temperature publisher
auto temp_node = librpc::createNode("temp_node");

// Node 2: Temperature subscriber
auto display_node = librpc::createNode("display_node");
display_node->subscribe("sensor", {"temperature"}, 
    [](const auto& group, const auto& topic, const auto* data, size_t size) {
        // Handle temperature
    });

// Publish temperature (display_node will receive in-process)
temp_node->broadcast("sensor", "temperature", "26.0C");
```

### Example 4: Inter-Process Communication

Process A:
```cpp
// Create node with specific UDP port
auto nodeA = librpc::createNode("nodeA", true, 47121);
nodeA->subscribe("ipc", {"commands"}, 
    [](const auto& group, const auto& topic, const auto* data, size_t size) {
        // Handle commands from other processes
    });
```

Process B:
```cpp
// Create node with different UDP port
auto nodeB = librpc::createNode("nodeB", true, 47122);
// Broadcast will be received by Process A via UDP
nodeB->broadcast("ipc", "commands", "START");
```

### Example 5: Selective Subscription

```cpp
auto node1 = librpc::createNode("node1");
auto node2 = librpc::createNode("node2");

// Node1 subscribes only to temperature
node1->subscribe("sensor", {"temperature"}, callback1);

// Node2 subscribes only to pressure
node2->subscribe("sensor", {"pressure"}, callback2);

// Only node1 receives this
node1->broadcast("sensor", "temperature", "25C");

// Only node2 receives this
node2->broadcast("sensor", "pressure", "1013hPa");
```

## Build Instructions

### Prerequisites

- C++14 or later
- Linux/QNX
- pthread
- socket support

### Compile

```bash
cd librpc
make
```

### Run Tests

LibRpc uses **SharedMemoryTransportV2** with lock-free SPSC queues for high-performance inter-process communication.

#### Quick Test (Recommended)
Run the complete test suite:
```bash
make run-tests
# Or directly:
./run_tests.sh
```

**Test Coverage:**
1. **In-process tests**: Basic operations + 20,000 message stress test
2. **Inter-process tests**: Sender/receiver performance validation
3. **Cleanup tests**: Automatic shared memory cleanup verification

#### Individual Tests

**1. In-Process Communication Test**
Tests multiple nodes within the same process using lock-free shared memory:
```bash
LD_LIBRARY_PATH=./lib ./test_inprocess basic
LD_LIBRARY_PATH=./lib ./test_inprocess stress
LD_LIBRARY_PATH=./lib ./test_inprocess all
```

**What This Tests:**
- Node registration in shared memory (max 8 nodes)
- In-process message delivery via SPSC queues
- Selective subscription (only matching subscribers receive messages)
- Stress test: 20,000 messages, ~493,000 msg/s throughput
- No message duplication, 100% delivery rate

**2. Inter-Process Communication Test**
Tests lock-free shared memory communication across processes:

Terminal 1 (Receiver):
```bash
LD_LIBRARY_PATH=./lib ./test_interprocess_receiver
```

Terminal 2 (Sender - start after receiver is ready):
```bash
LD_LIBRARY_PATH=./lib ./test_interprocess_sender
```

**What This Tests:**
- Cross-process SPSC queue communication
- Lock-free concurrent access (no mutex contention)
- Sender performance: ~1,362,000 msg/s
- Receiver performance: ~979 msg/s
- Shared memory: /dev/shm/librpc_shm_v2 (132MB)

**3. Automatic Cleanup Test**
Tests shared memory lifecycle management:
```bash
LD_LIBRARY_PATH=./lib ./test_cleanup
```

**What This Tests:**
- Last node triggers shm_unlink
- Heartbeat-based zombie node detection (5s timeout)
- Orphaned memory cleanup
- Multiple create/destroy cycles
- 6 test scenarios, all PASSED

## Message Protocol

### Packet Structure

```
+--------+--------+----------+----------+----------+----------+
| Magic  | Version| GroupLen | TopicLen | PayloadLen| Checksum|
| 4bytes | 2bytes | 2bytes   | 2bytes   | 4bytes   | 4bytes  |
+--------+--------+----------+----------+----------+----------+
| NodeID (64 bytes)                                           |
+-------------------------------------------------------------+
| Group Data (variable)                                       |
+-------------------------------------------------------------+
| Topic Data (variable)                                       |
+-------------------------------------------------------------+
| Payload Data (variable)                                     |
+-------------------------------------------------------------+
```

### UDP Configuration

- **Port assignment**: Each node binds to a unique UDP port
- **Node discovery**: Port scanning (localhost ports 47200-47230, 48000-48020)
- **Communication**: Point-to-point UDP based on discovered node addresses
- **Max message size**: ~64KB
- **Protocol**: Custom message format with checksums

## Performance Characteristics

### In-Process Communication

- **Latency**: < 1μs (direct function call)
- **Throughput**: > 1M msg/s (limited only by callback processing)
- **Memory**: Zero-copy

### Inter-Process Communication (Shared Memory V2)

- **Latency**: ~1-2μs (lock-free SPSC queue)
- **Throughput**: 
  - Send: ~1,000,000 msg/s (parallel write to multiple queues)
  - Receive: ~500,000 msg/s (polling from SPSC queues)
  - In-process: ~493,000 msg/s (full duplex)
- **Memory**: 132MB shared memory (8 nodes × 1024 msg/queue)
- **Architecture**: N×N SPSC queue matrix (64 queues for 8 nodes)
- **Advantages**: 
  - No mutex contention (lock-free)
  - Zero-copy (direct memory access)
  - Atomic operations only (write_pos, read_pos)
  - 89x faster than mutex-based approach

### Cross-Host Communication (UDP)

- **Latency**: ~100μs (localhost), higher for network
- **Throughput**: ~100K messages/sec
- **Memory**: One copy (serialization)
- **Use case**: Cross-subnet, cross-host messaging

## Thread Safety

- All public APIs are thread-safe
- Callbacks may be invoked from different threads
- Use proper synchronization in callbacks if needed

## Error Handling

```cpp
enum Error {
    NO_ERROR         = 0,
    INVALID_ARG      = 1,
    NOT_INITIALIZED  = 2,
    ALREADY_EXISTS   = 3,
    NOT_FOUND        = 4,
    NETWORK_ERROR    = 5,
    TIMEOUT          = 6,
    UNEXPECTED_ERROR = 99,
};
```

## Limitations

1. **Node limit**: Maximum 8 nodes per host (configurable via MAX_NODES)
2. **Queue capacity**: 1024 messages per queue (configurable via QUEUE_CAPACITY)
3. **Message size**: Single message limited to 2KB in shared memory (MESSAGE_SIZE)
4. **Shared memory**: Same-host only, does not work across network
5. **UDP limitations**: Broadcast may not work across subnets, ~64KB message size
6. **Delivery guarantee**: Best-effort (queue full = drop message)
7. **No encryption**: No built-in encryption/authentication

## Shared Memory Configuration

Edit `include/SharedMemoryTransportV2.h` to adjust:

```cpp
static constexpr int MAX_NODES = 8;          // Maximum nodes
static constexpr int QUEUE_CAPACITY = 1024;  // Messages per queue
static constexpr int MESSAGE_SIZE = 2048;    // Bytes per message
static constexpr int HEARTBEAT_INTERVAL = 1; // Seconds
static constexpr int NODE_TIMEOUT = 5;       // Seconds
```

**Memory calculation**: 
```
Total = MAX_NODES × MAX_NODES × QUEUE_CAPACITY × MESSAGE_SIZE
      = 8 × 8 × 1024 × 2048 bytes
      ≈ 132 MB
```

## Best Practices

1. **Use unique node IDs** for easier debugging
2. **Keep callbacks fast** to avoid blocking receive thread
3. **Subscribe before broadcast** in same-process scenarios
4. **Monitor queue capacity** - adjust QUEUE_CAPACITY if messages are dropped
5. **Limit node count** - stay within MAX_NODES (default 8)
6. **Handle large messages** - use UDP for messages > 2KB
7. **Check shared memory** - use `ls -lh /dev/shm/librpc_shm_v2` to monitor
8. **Clean shutdown** - let nodes exit gracefully for automatic cleanup
9. **Handle callback exceptions** to prevent crashes

## Troubleshooting

### Shared Memory Issues

**Problem**: "Failed to create shared memory"
```bash
# Check if memory already exists
ls -lh /dev/shm/librpc_shm_v2

# Manual cleanup (if nodes didn't exit gracefully)
rm /dev/shm/librpc_shm_v2

# Or use cleanup utility
LD_LIBRARY_PATH=./lib ./test_cleanup
```

**Problem**: Messages being dropped
- Increase `QUEUE_CAPACITY` in `SharedMemoryTransportV2.h`
- Recompile: `make clean && make`
- Trade-off: Higher capacity = more memory

**Problem**: "Too many nodes"
- Maximum is `MAX_NODES` (default 8)
- Increase in `SharedMemoryTransportV2.h` if needed
- Note: Memory usage grows as N²

### Performance Tuning

**Maximize throughput**:
```cpp
// In SharedMemoryTransportV2.h
static constexpr int QUEUE_CAPACITY = 2048;  // Double capacity
```

**Minimize memory**:
```cpp
static constexpr int MAX_NODES = 4;          // Fewer nodes
static constexpr int QUEUE_CAPACITY = 512;   // Smaller queues
// Memory: 4×4×512×2048 = 16MB
```

## Documentation

- **DESIGN.md**: Detailed architecture and design decisions
- **CODE_OPTIMIZATION_REPORT.md**: Performance optimization details (3-phase plan)
- **TEST_README.md**: Comprehensive test suite documentation
- **.backup/**: Legacy SharedMemoryTransport (mutex-based) for reference

## License

Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved

---

## Version History

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
node->broadcast("group", "topic", "data");
```

**Key Features**: Lock-free, high-performance (500K msg/s), auto-cleanup, 132MB footprint.
