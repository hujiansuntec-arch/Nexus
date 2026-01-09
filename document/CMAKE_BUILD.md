# LibRPC CMake 构建说明

## 📋 概述

LibRPC 现在使用 CMake 构建系统，提供更好的跨平台支持（Linux、QNX等）。

## 🚀 快速开始

### Linux 平台

```bash
# 方式1：使用便捷脚本（推荐）
./build.sh

# 方式2：手动使用 CMake
mkdir build && cd build
cmake ..
cmake --build . -j$(nproc)
```

### QNX 平台

```bash
# 设置 QNX 环境变量
export QNX_HOST=/opt/qnx710/host/linux/x86_64
export QNX_TARGET=/opt/qnx710/target/qnx7

# 使用便捷脚本
./build.sh -p qnx

# 或手动使用 CMake
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/qnx.cmake ..
cmake --build . -j$(nproc)
```

## 🔧 构建选项

### 使用 build.sh 脚本

```bash
# 查看帮助
./build.sh --help

# 常用选项
./build.sh                          # Release 模式，共享库
./build.sh -d                       # Debug 模式
./build.sh -c                       # 清理后重新构建
./build.sh -s                       # 构建静态库
./build.sh -t                       # 不构建测试
./build.sh -p qnx                   # QNX 交叉编译
./build.sh --install-prefix=/usr/local  # 自定义安装路径

# 组合使用
./build.sh -c -d -p qnx            # QNX, Debug, 清理构建
```

### 使用 CMake 直接配置

```bash
mkdir build && cd build

# Linux 构建
cmake -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DBUILD_TESTS=ON \
      -DCMAKE_INSTALL_PREFIX=./install \
      ..

# QNX 交叉编译
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=../cmake/qnx.cmake \
      -DBUILD_SHARED_LIBS=ON \
      -DBUILD_TESTS=ON \
      ..

# 构建
cmake --build . -j$(nproc)

# 安装
cmake --install .
```

## 📦 CMake 选项说明

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | Release | 构建类型：Debug/Release |
| `BUILD_SHARED_LIBS` | ON | 构建共享库（OFF=静态库） |
| `BUILD_TESTS` | ON | 构建测试程序 |
| `BUILD_EXAMPLES` | ON | 构建示例程序 |
| `CMAKE_INSTALL_PREFIX` | ./install | 安装路径 |

## 🏗️ 库结构说明

### 独立库：nexus_logger

从 v3.0 开始，Logger 模块被分离为独立的共享库 `libnexus_logger.so`，支持测试程序和用户应用直接使用。

**编译配置**（CMakeLists.txt Lines 42-71）：
```cmake
# Logger Library (standalone)
add_library(nexus_logger src/utils/Logger.cpp)
target_include_directories(nexus_logger PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nexus
)

if(BUILD_SHARED_LIBS)
    set_target_properties(nexus_logger PROPERTIES
        VERSION 3.0.0
        SOVERSION 3
        OUTPUT_NAME "nexus_logger"
        POSITION_INDEPENDENT_CODE ON
    )
endif()

target_link_libraries(nexus_logger pthread)
```

**编译产物**：
```
build/
├── libnexus_logger.so.3.0.0  # Logger完整版本库
├── libnexus_logger.so.3      # 主版本符号链接
├── libnexus_logger.so        # 开发符号链接
├── libnexus.so.3.0.0         # Nexus主库（依赖logger）
├── libnexus.so.3
└── libnexus.so
```

**依赖关系**：
- `libnexus.so` → `libnexus_logger.so` + pthread + rt
- `test_duplex_v2` → `libnexus.so` + `libnexus_logger.so`

**单独编译Logger库**：
```bash
cd build
make nexus_logger  # 只编译Logger库

# 验证
ls -lh libnexus_logger.so*
# libnexus_logger.so -> libnexus_logger.so.3
# libnexus_logger.so.3 -> libnexus_logger.so.3.0.0
# libnexus_logger.so.3.0.0

# 查看依赖
ldd libnexus_logger.so
# linux-vdso.so.1
# libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0
# libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
```

## 🧪 运行测试

```bash
cd build

# 设置库路径（如果使用共享库）
export LD_LIBRARY_PATH=$(pwd):$LD_LIBRARY_PATH

# 配置日志级别（可选）
export NEXUS_LOG_LEVEL=INFO  # DEBUG/INFO/WARN/ERROR/NONE

# 运行测试
./test_inprocess
./test_duplex_v2
./test_memory_config
./test_heartbeat_timeout

# 使用测试脚本（推荐）
cd ..
./run_dnexus_logger.so.3.0.0    # Logger独立库（新增）
├── libnexus_logger.so.3
├── libnexus_logger.so
├── libnexus.so.3.0.0           # Nexus主库
├── libnexus.so.3
├── libnexus.so
├── test_inprocess              # 测试程序（链接logger）
├── test_duplex_v2
├── test_memory_config
└── ...

install/                        # cmake --install . 的输出
├── lib/
│   ├── libnexus_logger.so.3.0.0
│   ├── libnexus_logger.so.3
│   ├── libnexus_logger.so
│   ├── libnexus.so.3.0.0
│   ├── libnexus.so.3
│   └── libnexus.so
└── include/
    └── nexus/
        ├── core/
        │   └── Node.h
        ├── transport/
        │   └── SharedMemoryTransportV3.h
        ├── utils/
        │   └── Logger.h            # Logger公共头文件
# 只显示关键信息（生产环境推荐）
export NEXUS_LOG_LEVEL=INFO
./run_duplex_test.sh multi 5 256 500 2 2

# 只显示错误
export NEXUS_LOG_LEVEL=ERROR
./run_duplex_test.sh multi 5 256 500 2 2

# 禁用日志（性能测试）
export NEXUS_LOG_LEVEL=NONE
./run_duplex_test.sh multi 20 256 1000 2 4
```

## 📂 构建输出

```
build/
├── librpc.so           # 共享库（或 librpc.a 静态库）
├── test_inprocess      # 测试程序
├── test_duplex_v2
├── test_memory_config
└── ...

install/                # cmake --install . 的输出
├── lib/
│   └── librpc.so
└── include/
    └── librpc/
        ├── Node.h
        ├── SharedMemoryTransportV3.h
        └── ...
```

## 🔄 从 Makefile 迁移

### 旧的 Makefile 命令 → 新的 CMake 命令

| Makefile | CMake |
|----------|-------|
| `make` | `./build.sh` 或 `cmake --build build` |
| `make clean` | `./build.sh -c` 或 `rm -rf build` |
| `make lib` | 默认构建库 |
| `make tests` | `cmake -DBUILD_TESTS=ON ..` |
| `make run-tests` | `cd build && ./test_inprocess` |

### 保留 Makefile

Makefile 仍然保留在项目中，你可以继续使用：

```bash
make          # 使用旧的 Makefile
./build.sh    # 使用新的 CMake
```

## 🌐 多平台构建

### 1. Linux 平台

```bash
# x86_64
./build.sh

# ARM64
cmake -DCMAKE_SYSTEM_PROCESSOR=aarch64 ..
```

### 2. QNX 平台

```bash
# AArch64
export QNX_HOST=/opt/qnx710/host/linux/x86_64
export QNX_TARGET=/opt/qnx710/target/qnx7
./build.sh -p qnx

# x86_64
# 修改 cmake/qnx.cmake 中的 CMAKE_SYSTEM_PROCESSOR
```

### 3. 交叉编译到其他 ARM 设备

```bash
# 自定义工具链文件
cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/your/toolchain.cmake ..
```

## 🛠️ IDE 集成

### Visual Studio Code

1. 安装 CMake Tools 扩展
2. 打开项目文件夹
3. Ctrl+Shift+P → "CMake: Configure"
4. Ctrl+Shift+P → "CMake: Build"

### CLion

1. 直接打开 CMakeLists.txt
2. CLion 自动识别 CMake 项目
3. 使用 Build 按钮构建

## 📊 性能对比

CMake vs Makefile 构建时间（4核CPU）：

| 构建方式 | 首次构建 | 增量构建 |
|---------|---------|----------|
| Makefile | ~3.5s | ~0.8s |
| CMake | ~3.2s | ~0.6s |

## ⚙️ 高级用法

### 1. 生成 Ninja 构建文件（更快）

```bash
cmake -G Ninja ..
ninja
```

### 2. 详细输出

```bash
cmake --build . --verbose
```

### 3. 只构建特定目标

```bash
cmake --build . --target rpc
cmake --build . --target test_inprocess
```

### 4. 并行构建

```bash
cmake --build . -j8  # 8个并行任务
```

### 5. 生成安装包

```bash
cd build
cpack                    # 生成 .tar.gz
cpack -G DEB            # 生成 .deb 包
cpack -G RPM            # 生成 .rpm 包
```

## 🐛 故障排除

### 问题1：找不到 rt 库

```bash
# 确认 rt 库存在
ldconfig -p | grep librt

# 如果缺失，安装
sudo apt-get install libc6-dev
```

### 问题2：QNX 环境未设置

```bash
# 设置 QNX 环境
export QNX_HOST=/opt/qnx710/host/linux/x86_64
export QNX_TARGET=/opt/qnx710/target/qnx7
export PATH=$QNX_HOST/usr/bin:$PATH
```

### 问题3：CMake 版本过低

```bash
# 检查版本
cmake --version

# 需要 CMake 3.10+
# Ubuntu 18.04+: sudo apt-get install cmake
# 或从 https://cmake.org/download/ 下载最新版
```

### 问题4：共享库找不到

```bash
# 设置运行时库路径
export LD_LIBRARY_PATH=$(pwd)/build:$LD_LIBRARY_PATH

# 或安装到系统路径
cd build && sudo cmake --install .
sudo ldconfig
```

## 📖 参考文档

- [CMake 官方文档](https://cmake.org/documentation/)
- [QNX CMake 指南](http://www.qnx.com/developers/docs/)
- [LibRPC QNX 兼容性](./QNX_COMPATIBILITY.md)

## 🤝 贡献

如果你发现构建问题或有改进建议，请提交 Issue 或 PR。
