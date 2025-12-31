# QNX 平台兼容性说明

## ✅ 已完成的兼容性修改

### 1. **MAP_NORESERVE 标志处理**
- **问题**：QNX 可能不支持 `MAP_NORESERVE` 标志
- **解决方案**：使用条件编译，QNX 平台使用 `MAP_SHARED` 而不带 `MAP_NORESERVE`
- **文件**：`src/SharedMemoryTransportV3.cpp`

```cpp
#ifdef __QNXNTO__
my_shm_ptr_ = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, 
                   MAP_SHARED, my_shm_fd_, 0);
#else
my_shm_ptr_ = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, 
                   MAP_SHARED | MAP_NORESERVE, my_shm_fd_, 0);
#endif
```

### 2. **共享内存路径差异**
- **问题**：Linux 使用 `/dev/shm`，QNX 使用 `/dev/shmem`
- **解决方案**：条件编译选择正确的路径
- **文件**：`src/SharedMemoryTransportV3.cpp`

```cpp
#ifdef __QNXNTO__
const char* shm_dir = "/dev/shmem";
#else
const char* shm_dir = "/dev/shm";
#endif
```

### 3. **头文件包含**
- 添加 `<cerrno>` 用于 errno
- QNX 平台添加 `<sys/neutrino.h>` 和 `<sys/procfs.h>`
- **文件**：`src/SharedMemoryTransportV3.cpp`, `src/SharedMemoryRegistry.cpp`

### 4. **兼容性头文件**
- 创建 `include/qnx_compat.h` 统一处理平台差异
- 提供宏定义简化跨平台代码

## 📋 QNX 编译要求

### 最低版本要求
- **QNX 7.0+** (支持 C++14)
- **QNX Momentics IDE** 或 **qcc 编译器**

### 编译命令

#### Linux 平台编译
```bash
make clean
make
```

#### QNX 平台编译
```bash
# 使用 qcc 编译器
export QCC=/opt/qnx700/host/linux/x86_64/usr/bin/qcc
make clean
CXX=$QCC make
```

或者在 Makefile 中设置：
```makefile
# 检测 QNX 平台
ifeq ($(shell uname -o 2>/dev/null),QNX)
    CXX = qcc
    CXXFLAGS += -D__QNXNTO__
endif
```

## 🔍 QNX 特定配置

### 共享内存配置
QNX 系统可能需要调整共享内存限制：

```bash
# 查看当前配置
shmctl

# 调整共享内存大小（需要 root 权限）
# 在 /etc/system/config/shmctl.conf 中设置
```

### 进程间通信
QNX 完全支持 POSIX 共享内存 API：
- `shm_open()` ✅
- `shm_unlink()` ✅
- `mmap()` ✅
- `munmap()` ✅
- `ftruncate()` ✅

## ⚠️ 注意事项

### 1. 内存占用
由于 QNX 不支持 `MAP_NORESERVE`，共享内存会立即占用物理内存：
- **Linux**：延迟分配，只有访问时才占用物理内存
- **QNX**：立即分配，创建时就占用全部物理内存

**建议**：在内存受限的 QNX 设备上，建议通过修改 `SharedMemoryTransportV3.h` 中的 `MAX_INBOUND_QUEUES`（如降至 16 或 8）并重新编译，以显著降低每个节点的物理内存占用（从 40MB 降至 ~10MB 或更低）。

**影响**：
- 33 MB 共享内存会立即占用 33 MB 物理RAM
- 10个进程 = 330 MB RAM 立即占用
- 建议根据系统 RAM 大小调整配置

**优化建议**：
```cpp
// 对于内存受限的 QNX 系统，使用更小的配置
SharedMemoryTransportV3::Config config;
config.max_inbound_queues = 16;  // 减少到 16
config.queue_capacity = 128;     // 减少到 128
// 内存占用: 16 × 128 × 2112 ≈ 4.1 MB
```

### 2. 实时性能
QNX 是实时操作系统，考虑：
- 设置合适的线程优先级
- 避免在关键路径上的动态内存分配
- 使用 CPU 亲和性绑定关键线程

### 3. 进程优先级
```cpp
// QNX 特定：设置实时优先级
#ifdef __QNXNTO__
#include <sched.h>
struct sched_param param;
param.sched_priority = 10;  // 1-255
pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif
```

## 📊 性能对比

| 特性 | Linux | QNX |
|------|-------|-----|
| 共享内存延迟分配 | ✅ 支持 | ❌ 不支持 |
| POSIX 共享内存 | ✅ 完整支持 | ✅ 完整支持 |
| C++14 原子操作 | ✅ 支持 | ✅ 支持 |
| Lock-free 队列 | ✅ 高性能 | ✅ 高性能 |
| 实时调度 | ⚠️ 软实时 | ✅ 硬实时 |

## 🧪 测试

### 基本功能测试
```bash
# 在 QNX 上运行测试
./test_inprocess
./test_duplex_v2
./test_memory_config
```

### 验证共享内存
```bash
# QNX 查看共享内存
ls -lh /dev/shmem/

# 查看系统资源
pidin mem
```

## 📝 已验证的 QNX 版本
- ✅ QNX 7.0.0
- ✅ QNX 7.1.0
- ⏳ QNX 8.0 (待测试)

## 🔗 相关文档
- [QNX Neutrino RTOS Documentation](http://www.qnx.com/developers/docs/)
- [QNX IPC Guide](http://www.qnx.com/developers/docs/7.1/#com.qnx.doc.neutrino.prog/topic/ipc.html)
- [POSIX Shared Memory](http://pubs.opengroup.org/onlinepubs/9699919799/)
