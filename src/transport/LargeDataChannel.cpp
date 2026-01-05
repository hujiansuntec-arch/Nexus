// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
//
// LargeDataChannel 实现 - 高频大数据传输专用通道

#include "nexus/transport/LargeDataChannel.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>  // 用于 kill() 检测进程存活
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <stdexcept>

#include "nexus/utils/Logger.h"

// ============ LargeDataChannel Constants ============
#define LARGE_DATA_CLEANUP_INTERVAL_S 30  // Dead reader cleanup interval (seconds)
#define LARGE_DATA_READER_TIMEOUT_S 60    // Reader heartbeat timeout (seconds)
#define LARGE_DATA_MIN_VALID_SIZE 4096    // Minimum valid channel size (bytes)

namespace Nexus {
namespace rpc {

// 全局标志：是否已执行过启动清理
static std::atomic<bool> g_cleanup_done{false};
static std::mutex g_cleanup_mutex;

// CRC32-C (Castagnoli) 查找表 - 用于软件回退
static const uint32_t crc32c_table[256] = {
    0x00000000, 0xf26b8303, 0xe13b70f7, 0x1350f3f4, 0xc79a971f, 0x35f1141c, 0x26a1e7e8, 0xd4ca64eb, 0x8ad958cf, 0x78b2dbcc, 0x6be22838, 0x9989ab3b, 0x4d43cfd0, 0xbf284cd3, 0xac78bf27, 0x5e133c24, 0x105ec76f, 0xe235446c, 0xf165b798, 0x030e349b, 0xd7c45070, 0x25afd373, 0x36ff2087, 0xc494a384, 0x9a879fa0, 0x68ec1ca3, 0x7bbcef57, 0x89d76c54, 0x5d1d08bf, 0xaf768bbc, 0xbc267848, 0x4e4dfb4b, 0x20bd8ede, 0xd2d60ddd, 0xc186fe29, 0x33ed7d2a, 0xe72719c1, 0x154c9ac2, 0x061c6936, 0xf477ea35, 0xaa64d611, 0x580f5512, 0x4b5fa6e6, 0xb93425e5, 0x6dfe410e, 0x9f95c20d, 0x8cc531f9, 0x7eaeb2fa, 0x30e349b1, 0xc288cab2, 0xd1d83946, 0x23b3ba45, 0xf779deae, 0x05125dad, 0x1642ae59, 0xe4292d5a, 0xba3a117e, 0x4851927d, 0x5b016189, 0xa96ae28a, 0x7da08661, 0x8fcb0562, 0x9c9bf696, 0x6ef07595, 0x417b1dbc, 0xb3109ebf, 0xa0406d4b, 0x522bee48, 0x86e18aa3, 0x748a09a0, 0x67dafa54, 0x95b17957, 0xcba24573, 0x39c9c670, 0x2a993584, 0xd8f2b687, 0x0c38d26c, 0xfe53516f, 0xed03a29b, 0x1f682198, 0x5125dad3, 0xa34e59d0, 0xb01eaa24, 0x42752927, 0x96bf4dcc, 0x64d4cecf, 0x77843d3b, 0x85efbe38, 0xdbfc821c, 0x2997011f, 0x3ac7f2eb, 0xc8ac71e8, 0x1c661503, 0xee0d9600, 0xfd5d65f4, 0x0f36e6f7, 0x61c69362, 0x93ad1061, 0x80fde395, 0x72966096, 0xa65c047d, 0x5437877e, 0x4767748a, 0xb50cf789, 0xeb1fcbad, 0x197448ae, 0x0a24bb5a, 0xf84f3859, 0x2c855cb2, 0xdeeedfb1, 0xcdbe2c45, 0x3fd5af46, 0x7198540d, 0x83f3d70e, 0x90a324fa, 0x62c8a7f9, 0xb602c312, 0x44694011, 0x5739b3e5, 0xa55230e6, 0xfb410cc2, 0x092a8fc1, 0x1a7a7c35, 0xe811ff36, 0x3cdb9bdd, 0xceb018de, 0xdde0eb2a, 0x2f8b6829, 0x82f63b78, 0x709db87b, 0x63cd4b8f, 0x91a6c88c, 0x456cac67, 0xb7072f64, 0xa457dc90, 0x563c5f93, 0x082f63b7, 0xfa44e0b4, 0xe9141340, 0x1b7f9043, 0xcfb5f4a8, 0x3dde77ab, 0x2e8e845f, 0xdce5075c, 0x92a8fc17, 0x60c37f14, 0x73938ce0, 0x81f80fe3, 0x55326b08, 0xa759e80b, 0xb4091bff, 0x466298fc, 0x1871a4d8, 0xea1a27db, 0xf94ad42f, 0x0b21572c, 0xdfeb33c7, 0x2d80b0c4, 0x3ed04330, 0xccbbc033, 0xa24bb5a6, 0x502036a5, 0x4370c551, 0xb11b4652, 0x65d122b9, 0x97baa1ba, 0x84ea524e, 0x7681d14d, 0x2892ed69, 0xdaf96e6a, 0xc9a99d9e, 0x3bc21e9d, 0xef087a76, 0x1d63f975, 0x0e330a81, 0xfc588982, 0xb21572c9, 0x407ef1ca, 0x532e023e, 0xa145813d, 0x758fe5d6, 0x87e466d5, 0x94b49521, 0x66df1622, 0x38cc2a06, 0xcaa7a905, 0xd9f75af1, 0x2b9cd9f2, 0xff56bd19, 0x0d3d3e1a, 0x1e6dcdee, 0xec064eed, 0xc38d26c4, 0x31e6a5c7, 0x22b65633, 0xd0ddd530, 0x0417b1db, 0xf67c32d8, 0xe52cc12c, 0x1747422f, 0x49547e0b, 0xbb3ffd08, 0xa86f0efc, 0x5a048dff, 0x8ecee914, 0x7ca56a17, 0x6ff599e3, 0x9d9e1ae0, 0xd3d3e1ab, 0x21b862a8, 0x32e8915c, 0xc083125f, 0x144976b4, 0xe622f5b7, 0xf5720643, 0x07198540, 0x590ab964, 0xab613a67, 0xb831c993, 0x4a5a4a90, 0x9e902e7b, 0x6cfbad78, 0x7fab5e8c, 0x8dc0dd8f, 0xe330a81a, 0x115b2b19, 0x020bd8ed, 0xf0605bee, 0x24aa3f05, 0xd6c1bc06, 0xc5914ff2, 0x37faccf1, 0x69e9f0d5, 0x9b8273d6, 0x88d28022, 0x7ab90321, 0xae7367ca, 0x5c18e4c9, 0x4f48173d, 0xbd23943e, 0xf36e6f75, 0x0105ec76, 0x12551f82, 0xe03e9c81, 0x34f4f86a, 0xc69f7b69, 0xd5cf889d, 0x27a40b9e, 0x79b737ba, 0x8bdcb4b9, 0x988c474d, 0x6ae7c44e, 0xbe2da0a5, 0x4c4623a6, 0x5f16d052, 0xad7d5351};

// Hardware CRC32 implementation
static uint32_t calculateCRC32HW(const uint8_t* data, size_t size) {
#ifdef __SSE4_2__
    uint32_t crc = 0xFFFFFFFF;

    // Process 8 bytes at a time
    size_t i = 0;
#ifdef __x86_64__
    for (; i + 8 <= size; i += 8) {
        uint64_t val;
        memcpy(&val, data + i, 8);
        crc = (uint32_t)_mm_crc32_u64(crc, val);
    }
#endif
    // Process 4 bytes at a time
    for (; i + 4 <= size; i += 4) {
        uint32_t val;
        memcpy(&val, data + i, 4);
        crc = _mm_crc32_u32(crc, val);
    }
    // Process remaining bytes
    for (; i < size; i++) {
        crc = _mm_crc32_u8(crc, data[i]);
    }

    return ~crc;
#else
    return 0;
#endif
}

// Check for SSE4.2 support
static bool hasSSE42() {
    static bool checked = false;
    static bool supported = false;
    if (!checked) {
#ifdef __SSE4_2__
        // Runtime check using __builtin_cpu_supports (GCC/Clang)
        #if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
            supported = __builtin_cpu_supports("sse4.2");
        #else
            supported = true;
        #endif
#else
        supported = false;
#endif
        checked = true;
        if (supported) {
             NEXUS_INFO("LargeData") << "Hardware CRC32 (SSE4.2) enabled";
        }
    }
    return supported;
}

// 计算CRC32
uint32_t LargeDataChannel::calculateCRC32(const uint8_t* data, size_t size) const {
    if (hasSSE42()) {
        return calculateCRC32HW(data, size);
    }

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// 创建或连接到大数据通道
std::shared_ptr<LargeDataChannel> LargeDataChannel::create(const std::string& shm_name, const Config& config) {
    // 首次创建时自动清理过期通道（进程级别只执行一次）
    if (!g_cleanup_done.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_cleanup_mutex);
        // 双重检查
        if (!g_cleanup_done.load(std::memory_order_relaxed)) {
            NEXUS_DEBUG("LargeData") << "First channel creation, performing startup cleanup...";
            size_t cleaned = cleanupOrphanedChannels(60);
            if (cleaned > 0) {
                NEXUS_DEBUG("LargeData") << "Startup cleanup: removed " << cleaned << " orphaned channel(s)";
            } else {
                NEXUS_DEBUG("LargeData") << "Startup cleanup: no orphaned channels found";
            }
            g_cleanup_done.store(true, std::memory_order_release);
        }
    }

    auto channel = std::shared_ptr<LargeDataChannel>(new LargeDataChannel(shm_name, config));

    if (!channel->initialize()) {
        return nullptr;
    }

    return channel;
}

LargeDataChannel::LargeDataChannel(const std::string& shm_name, const Config& config)
    : shm_name_(shm_name),
      config_(config),
      shm_fd_(-1),
      shm_addr_(nullptr),
      shm_size_(0),
      control_(nullptr),
      buffer_(nullptr),
      reader_id_(-1),  // 初始化为-1（未注册）
      total_writes_(0),
      total_reads_(0),
      total_bytes_written_(0),
      total_bytes_read_(0),
      total_dropped_(0) {}

LargeDataChannel::~LargeDataChannel() {
    // 如果是读者，注销读者槽位
    if (reader_id_ >= 0) {
        unregisterReader(reader_id_);
    }

    // 递减引用计数
    if (control_ && control_->ref_count.load(std::memory_order_acquire) > 0) {
        int32_t prev_count = control_->ref_count.fetch_sub(1, std::memory_order_acq_rel);

        NEXUS_DEBUG("LargeData") << "Destructor: " << shm_name_ << ", ref_count: " << prev_count << " -> "
                                 << (prev_count - 1);

        // 如果是最后一个引用，清理共享内存
        if (prev_count == 1) {
            NEXUS_DEBUG("LargeData") << "Last reference, unlinking shared memory: " << shm_name_;

            // 取消映射
            if (shm_addr_ != nullptr && shm_addr_ != MAP_FAILED) {
                munmap(shm_addr_, shm_size_);
                shm_addr_ = nullptr;
            }

            // 关闭文件描述符
            if (shm_fd_ >= 0) {
                close(shm_fd_);
                shm_fd_ = -1;
            }

            // 删除共享内存对象
            if (shm_unlink(shm_name_.c_str()) == 0) {
                NEXUS_DEBUG("LargeData") << "Successfully unlinked: " << shm_name_;
            } else {
                NEXUS_ERROR("LargeData") << "Failed to unlink: " << shm_name_ << " (errno: " << errno << ")";
            }

            return;
        }
    }

    // 不是最后一个引用，只取消映射和关闭fd
    if (shm_addr_ != nullptr && shm_addr_ != MAP_FAILED) {
        munmap(shm_addr_, shm_size_);
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
    }
}

bool LargeDataChannel::initialize() {
    // 计算共享内存大小（控制块 + 缓冲区）
    shm_size_ = sizeof(RingBufferControl) + config_.buffer_size;

    // 对齐到页大小
    size_t page_size = sysconf(_SC_PAGESIZE);
    shm_size_ = ((shm_size_ + page_size - 1) / page_size) * page_size;

    std::string shm_path = "/dev/shm/" + shm_name_;

    // 尝试创建或打开共享内存
    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd_ < 0) {
        NEXUS_ERROR("LargeData") << "Failed to open shared memory: " << shm_name_;
        return false;
    }

    // 🔒 核心修复：获取共享锁，标记该通道正在被使用
    // 当进程崩溃时，操作系统会自动释放该锁
    if (flock(shm_fd_, LOCK_SH) < 0) {
        NEXUS_ERROR("LargeData") << "Failed to lock shared memory: " << strerror(errno);
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    // 获取当前大小
    struct stat st;
    if (fstat(shm_fd_, &st) < 0) {
        NEXUS_ERROR("LargeData") << "Failed to stat shared memory";
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    // 如果是新创建的（大小为0），设置大小并初始化
    bool is_creator = (st.st_size == 0);

    if (is_creator) {
        if (ftruncate(shm_fd_, shm_size_) < 0) {
            NEXUS_ERROR("LargeData") << "Failed to resize shared memory";
            close(shm_fd_);
            shm_fd_ = -1;
            return false;
        }
    }

    // 映射共享内存（使用MAP_NORESERVE优化）
    int mmap_flags = MAP_SHARED;
    if (config_.use_mmap_noreserve) {
        mmap_flags |= MAP_NORESERVE;
    }

    shm_addr_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, mmap_flags, shm_fd_, 0);

    if (shm_addr_ == MAP_FAILED) {
        NEXUS_ERROR("LargeData") << "Failed to mmap shared memory";
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    // 设置控制块和缓冲区指针
    control_ = reinterpret_cast<RingBufferControl*>(shm_addr_);
    buffer_ = reinterpret_cast<uint8_t*>(shm_addr_) + sizeof(RingBufferControl);

    // 如果是创建者，初始化控制块
    if (is_creator) {
        control_->write_pos.store(0);
        control_->sequence.store(0);
        control_->ref_count.store(1, std::memory_order_release);          // 初始引用计数为1
        control_->writer_pid.store(getpid(), std::memory_order_release);  // 记录写端PID
        control_->num_readers.store(0, std::memory_order_release);        // 读者数量初始化为0
        control_->capacity = config_.buffer_size;
        // Safe cast: config values are validated and fit in uint32_t
        control_->max_block_size = static_cast<uint32_t>(config_.max_block_size);
        control_->max_readers = static_cast<uint32_t>(config_.max_readers);

        // 初始化所有读者槽位
        for (size_t i = 0; i < MAX_READERS; ++i) {
            control_->readers[i].read_pos.store(0);
            control_->readers[i].heartbeat.store(0);
            control_->readers[i].pid.store(0);
            control_->readers[i].active.store(false);
        }

        NEXUS_DEBUG("LargeData") << "Created channel: " << shm_name_ << ", size: " << (shm_size_ / (1024 * 1024))
                                 << " MB"
                                 << ", max_readers: " << control_->max_readers
                                 << ", MAP_NORESERVE: " << (config_.use_mmap_noreserve ? "yes" : "no")
                                 << ", PID: " << getpid();
    } else {
        // 连接者：递增引用计数
        int32_t new_count = control_->ref_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        NEXUS_DEBUG("LargeData") << "Connected to channel: " << shm_name_ << ", ref_count: " << new_count
                                 << ", PID: " << getpid();
    }

    return true;
}

// 零拷贝写入：分配空间
LargeDataChannel::WritableBlock LargeDataChannel::allocWrite(size_t size) {
    WritableBlock block;
    if (size > config_.max_block_size) {
        NEXUS_ERROR("LargeData") << "Data size " << size << " exceeds max block size " << config_.max_block_size;
        return block;
    }

    // 🔧 优化2：数据对齐 (64字节对齐，Cache Line友好)
    size_t total_size = sizeof(LargeDataHeader) + size;
    size_t aligned_total_size = (total_size + 63) & ~63;

    // 清理死亡的读者（定期执行）
    cleanupDeadReaders();

    // 检查可用空间（基于所有读者中的最小read_pos）
    uint64_t min_read_pos = getMinReadPos();
    uint64_t write_pos = control_->write_pos.load(std::memory_order_acquire);
    uint64_t used = write_pos - min_read_pos;

    // 缓冲区满时的处理策略
    if (used + aligned_total_size > control_->capacity) {
        switch (config_.overflow_policy) {
            case LargeDataOverflowPolicy::DROP_OLDEST: {
                // 循环直到有足够空间
                int max_loops = 1000;  // 防止死循环
                while (used + aligned_total_size > control_->capacity && max_loops-- > 0) {
                    uint64_t current_min_pos = min_read_pos;
                    uint64_t read_offset = current_min_pos % control_->capacity;

                    // 1. 处理环绕情况 (Wrap-around)
                    if (read_offset + sizeof(LargeDataHeader) > control_->capacity) {
                        uint64_t skip = control_->capacity - read_offset;
                        // 强制所有在末尾的读者跳到开头
                        for (size_t i = 0; i < MAX_READERS; ++i) {
                            auto& reader = control_->readers[i];
                            if (reader.active.load(std::memory_order_acquire)) {
                                uint64_t rpos = reader.read_pos.load(std::memory_order_acquire);
                                if (rpos == current_min_pos) {
                                    reader.read_pos.compare_exchange_strong(rpos, current_min_pos + skip);
                                }
                            }
                        }
                        // 重新计算 min_read_pos
                        min_read_pos = getMinReadPos();
                        used = write_pos - min_read_pos;
                        continue;
                    }

                    // 2. 读取头部信息
                    LargeDataHeader* header = reinterpret_cast<LargeDataHeader*>(buffer_ + read_offset);
                    uint32_t magic =
                        reinterpret_cast<std::atomic<uint32_t>*>(&header->magic)->load(std::memory_order_acquire);

                    size_t drop_size = 0;
                    bool valid_header = false;

                    if (magic == LargeDataHeader::MAGIC && header->size <= config_.max_block_size) {
                        size_t total_size = sizeof(LargeDataHeader) + header->size;
                        drop_size = (total_size + 63) & ~63;
                        valid_header = true;
                    } else {
                        // 头部无效（可能是数据损坏或未初始化区域）
                        // 这种情况下，为了恢复，我们尝试跳过一个最小对齐单位
                        // 如果是在缓冲区末尾遇到的无效头部，很可能是Padding，直接跳到末尾
                        drop_size = control_->capacity - read_offset;
                        if (drop_size < 64) drop_size = 64; // 至少跳过64字节
                    }

                    // 3. 强制推进慢速读者
                    bool any_advanced = false;
                    for (size_t i = 0; i < MAX_READERS; ++i) {
                        auto& reader = control_->readers[i];
                        if (reader.active.load(std::memory_order_acquire)) {
                            uint64_t rpos = reader.read_pos.load(std::memory_order_acquire);
                            if (rpos == current_min_pos) {
                                if (reader.read_pos.compare_exchange_strong(rpos, current_min_pos + drop_size)) {
                                    any_advanced = true;
                                }
                            }
                        }
                    }

                    if (any_advanced && valid_header) {
                        size_t dropped = total_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
                        // Callback
                        if (config_.overflow_callback) {
                            try {
                                config_.overflow_callback(shm_name_, header->topic, header->sequence, dropped);
                            } catch (...) {
                            }
                        }
                    }

                    // 重新计算
                    min_read_pos = getMinReadPos();
                    used = write_pos - min_read_pos;
                }

                if (max_loops <= 0) {
                    NEXUS_ERROR("LargeData") << "Failed to free space after 1000 attempts (DROP_OLDEST)";
                    return block;  // Failed
                }
                break;
            }

            case LargeDataOverflowPolicy::DROP_NEWEST: {
                size_t dropped = total_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
                NEXUS_ERROR("LargeData") << "Buffer full, dropping newest data (total: " << dropped << ")";
                if (config_.overflow_callback) {
                    try {
                        uint64_t seq = control_->sequence.load(std::memory_order_relaxed);
                        config_.overflow_callback(shm_name_, "", seq, dropped);
                    } catch (...) {
                    }
                }
                return block;
            }

            case LargeDataOverflowPolicy::BLOCK:
                NEXUS_ERROR("LargeData") << "Buffer full (BLOCK policy not recommended)";
                return block;
        }
    }

    // 更新写端心跳
    uint64_t current_time = static_cast<uint64_t>(time(nullptr));
    control_->writer_heartbeat.store(current_time, std::memory_order_relaxed);

    // 获取序列号
    block.sequence = control_->sequence.fetch_add(1);

    // 计算写入位置（环形缓冲区）
    uint64_t write_offset = write_pos % control_->capacity;

    // 检查是否需要环绕
    if (write_offset + aligned_total_size > control_->capacity) {
        // 环绕到开头（浪费剩余空间）
        uint64_t skip_size = control_->capacity - write_offset;
        control_->write_pos.fetch_add(skip_size);
        write_pos = control_->write_pos.load();
        write_offset = 0;

        // 通知所有读者跳过浪费的空间
        for (size_t i = 0; i < MAX_READERS; ++i) {
            if (control_->readers[i].active.load(std::memory_order_acquire)) {
                uint64_t reader_pos = control_->readers[i].read_pos.load(std::memory_order_acquire);
                // 检查读者是否在被跳过的区域内
                if (reader_pos < write_pos && (reader_pos % control_->capacity) >= (control_->capacity - skip_size)) {
                    control_->readers[i].read_pos.store(write_pos, std::memory_order_release);
                    NEXUS_WARN("LargeData")
                        << "Reader #" << i << " skipped " << skip_size << " bytes due to ring wrap (from " << reader_pos
                        << " to " << write_pos << ")";
                }
            }
        }
    }

    block.data = buffer_ + write_offset + sizeof(LargeDataHeader);
    block.size = size;
    block.write_offset = write_offset;

    return block;
}

// 零拷贝写入：提交
int64_t LargeDataChannel::commitWrite(const WritableBlock& block, const std::string& topic) {
    if (!block.isValid()) return -1;

    // 重新计算对齐大小
    size_t total_size = sizeof(LargeDataHeader) + block.size;
    size_t aligned_total_size = (total_size + 63) & ~63;

    // 准备头部
    LargeDataHeader* header = reinterpret_cast<LargeDataHeader*>(buffer_ + block.write_offset);
    header->magic = 0;  // 暂时设为0
    header->size = static_cast<uint32_t>(block.size);
    header->sequence = block.sequence;
    strncpy(header->topic, topic.c_str(), sizeof(header->topic) - 1);
    header->topic[sizeof(header->topic) - 1] = '\0';

    // 🔧 优化3：CRC32配置化
    if (config_.enable_crc32) {
        header->crc32 = calculateCRC32(block.data, block.size);
    } else {
        header->crc32 = 0;
    }

    // 内存屏障
    std::atomic_thread_fence(std::memory_order_release);

    // 写入Magic
    reinterpret_cast<std::atomic<uint32_t>*>(&header->magic)->store(LargeDataHeader::MAGIC, std::memory_order_release);

    // 更新写指针
    control_->write_pos.fetch_add(aligned_total_size, std::memory_order_release);

    // 更新统计
    total_writes_.fetch_add(1);
    total_bytes_written_.fetch_add(block.size);

    return block.sequence;
}

// 写入大数据（兼容旧接口）
int64_t LargeDataChannel::write(const std::string& topic, const uint8_t* data, size_t size) {
    WritableBlock block = allocWrite(size);
    if (!block.isValid()) return -1;

    // 内存拷贝
    memcpy(block.data, data, size);

    return commitWrite(block, topic);
}

// 尝试读取数据块
bool LargeDataChannel::tryRead(DataBlock& block) {
    // 如果还未注册为读者，先注册
    if (reader_id_ < 0) {
        reader_id_ = registerReader();
        if (reader_id_ < 0) {
            NEXUS_ERROR("LargeData") << "Failed to register as reader (max readers exceeded)";
            return false;
        }
    }

    // 更新读端心跳
    updateReaderHeartbeat(reader_id_);

    // 使用acquire语义读取写指针和当前读者的read_pos
    uint64_t read_pos = control_->readers[reader_id_].read_pos.load(std::memory_order_acquire);
    uint64_t write_pos = control_->write_pos.load(std::memory_order_acquire);

    // 🔧 关键优化：检查read_pos是否指向已被覆盖的数据
    // 在环形缓冲区中，有效数据范围是 [min_read_pos, write_pos)
    uint64_t min_read_pos = getMinReadPos();
    if (read_pos < min_read_pos) {
        // read_pos指向的数据已被覆盖，跳到当前可读的最早位置
        control_->readers[reader_id_].read_pos.store(min_read_pos, std::memory_order_release);
        read_pos = min_read_pos;
        NEXUS_WARN("LargeData") << "Reader #" << reader_id_ << " read_pos was behind, adjusted from 0 to "
                                << min_read_pos;
    }

    // 检查是否有数据
    if (read_pos >= write_pos) {
        return false;
    }

    // 内存屏障：确保后续读取看到最新数据
    std::atomic_thread_fence(std::memory_order_acquire);

    // 计算读取位置
    uint64_t read_offset = read_pos % control_->capacity;
    size_t available = write_pos - read_pos;

    // 检查是否有足够的数据读取头部
    if (available < sizeof(LargeDataHeader)) {
        return false;
    }

    // 读取头部
    const LargeDataHeader* header = reinterpret_cast<const LargeDataHeader*>(buffer_ + read_offset);

    // 验证数据块（包括检查magic是否已写入）
    ReadResult validation_result = validateBlock(header, available);

    if (validation_result != ReadResult::SUCCESS) {
        // 根据错误类型决定是否跳过
        if (validation_result == ReadResult::INVALID_MAGIC || validation_result == ReadResult::INSUFFICIENT) {
            // 数据可能还在写入，不跳过，等待下次读取
            block.result = validation_result;
            return false;
        } else {
            // SIZE_EXCEEDED或CRC_ERROR，跳过这个数据块
            size_t skip_size = sizeof(LargeDataHeader);
            if (validation_result == ReadResult::CRC_ERROR) {
                // 如果是CRC错误，说明头部有效，可以跳过整个块
                size_t total_size = sizeof(LargeDataHeader) + header->size;
                skip_size = (total_size + 63) & ~63;
            }
            control_->readers[reader_id_].read_pos.fetch_add(skip_size, std::memory_order_release);
            block.result = validation_result;
            return false;
        }
    }

    // 填充DataBlock
    block.header = header;
    block.data = buffer_ + read_offset + sizeof(LargeDataHeader);
    block.size = header->size;
    block.result = ReadResult::SUCCESS;

    return true;
}

// 释放数据块
void LargeDataChannel::releaseBlock(const DataBlock& block) {
    if (!block.isValid() || reader_id_ < 0) {
        return;
    }

    // 更新当前读者的read_pos（使用release语义）
    size_t total_size = sizeof(LargeDataHeader) + block.size;
    size_t aligned_total_size = (total_size + 63) & ~63;
    control_->readers[reader_id_].read_pos.fetch_add(aligned_total_size, std::memory_order_release);

    // 更新统计
    total_reads_.fetch_add(1, std::memory_order_relaxed);
    total_bytes_read_.fetch_add(block.size, std::memory_order_relaxed);
}

// 验证数据块（返回详细错误）
LargeDataChannel::ReadResult LargeDataChannel::validateBlock(const LargeDataHeader* header, size_t available) const {
    // 检查魔数（可能数据还未写完）
    // 必须使用atomic load确保跨进程可见性
    uint32_t magic = reinterpret_cast<const std::atomic<uint32_t>*>(&header->magic)->load(std::memory_order_acquire);
    if (magic != LargeDataHeader::MAGIC) {
        return ReadResult::INVALID_MAGIC;
    }

    // 检查大小
    if (header->size > config_.max_block_size) {
        NEXUS_ERROR("LargeData") << "Size exceeded: " << header->size << " > " << config_.max_block_size;
        return ReadResult::SIZE_EXCEEDED;
    }

    // 检查是否有足够的数据
    size_t total_size = sizeof(LargeDataHeader) + header->size;
    if (available < total_size) {
        return ReadResult::INSUFFICIENT;
    }

    // 验证CRC32
    if (config_.enable_crc32) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(header) + sizeof(LargeDataHeader);
        uint32_t calculated_crc = calculateCRC32(data, header->size);

        if (calculated_crc != header->crc32) {
            NEXUS_ERROR("LargeData") << "CRC32 mismatch: expected " << header->crc32 << ", got " << calculated_crc;
            return ReadResult::CRC_ERROR;
        }
    }

    return ReadResult::SUCCESS;
}

// 获取统计信息
LargeDataChannel::Stats LargeDataChannel::getStats() const {
    Stats stats;
    stats.total_writes = total_writes_.load();
    stats.total_reads = total_reads_.load();
    stats.total_bytes_written = total_bytes_written_.load();
    stats.total_bytes_read = total_bytes_read_.load();

    uint64_t write_pos = control_->write_pos.load();
    uint64_t min_read_pos = getMinReadPos();
    stats.current_usage = (write_pos > min_read_pos) ? (write_pos - min_read_pos) : 0;
    stats.capacity = control_->capacity;

    return stats;
}

// 获取可用空间
size_t LargeDataChannel::getAvailableSpace() const {
    uint64_t write_pos = control_->write_pos.load();
    uint64_t min_read_pos = getMinReadPos();

    uint64_t used = write_pos - min_read_pos;

    if (used >= control_->capacity) {
        return 0;
    }

    return control_->capacity - used;
}

// 检查是否可以写入
bool LargeDataChannel::canWrite(size_t size) const {
    size_t total_size = sizeof(LargeDataHeader) + size;
    size_t aligned_total_size = (total_size + 63) & ~63;

    // 如果策略是 DROP_OLDEST 或 DROP_NEWEST，只要单块大小不超过容量，总是可以写入
    if (config_.overflow_policy == LargeDataOverflowPolicy::DROP_OLDEST ||
        config_.overflow_policy == LargeDataOverflowPolicy::DROP_NEWEST) {
        return aligned_total_size <= control_->capacity;
    }

    return getAvailableSpace() >= aligned_total_size;
}

// 设置溢出策略
void LargeDataChannel::setOverflowPolicy(LargeDataOverflowPolicy policy) {
    config_.overflow_policy = policy;
    NEXUS_DEBUG("LargeData") << "Overflow policy set to: "
                             << (policy == LargeDataOverflowPolicy::DROP_OLDEST   ? "DROP_OLDEST"
                                 : policy == LargeDataOverflowPolicy::DROP_NEWEST ? "DROP_NEWEST"
                                                                                  : "BLOCK");
}

// 设置溢出回调
void LargeDataChannel::setOverflowCallback(LargeDataOverflowCallback callback) {
    config_.overflow_callback = callback;
    NEXUS_DEBUG("LargeData") << "Overflow callback " << (callback ? "enabled" : "disabled");
}

// 清理过期的大数据通道（静态函数）
size_t LargeDataChannel::cleanupOrphanedChannels(uint32_t /*timeout_seconds*/) {
    NEXUS_DEBUG("LargeData") << "Scanning for orphaned channels (using file locks)...";

    DIR* dir = opendir("/dev/shm");
    if (!dir) {
        NEXUS_ERROR("LargeData") << "Failed to open /dev/shm: " << strerror(errno);
        return 0;
    }

    struct dirent* entry;
    size_t cleaned_count = 0;
    size_t total_freed = 0;

    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // 只处理包含"channel"的共享内存（排除V3的librpc_node_*）
        if (name.find("channel") == std::string::npos) {
            continue;
        }

        // 尝试打开共享内存
        int fd = shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            continue;
        }

        // 获取文件大小用于统计
        struct stat st;
        size_t shm_size = 0;
        if (fstat(fd, &st) == 0) {
            shm_size = st.st_size;
        }

        // 🔒 核心修复：尝试获取排他锁
        // 如果能获取到排他锁，说明没有任何进程持有共享锁（即没有进程在使用该通道）
        // LOCK_NB 确保不阻塞
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            // 成功获取排他锁，说明是僵尸文件，可以安全删除
            if (shm_unlink(name.c_str()) == 0) {
                cleaned_count++;
                total_freed += shm_size;
                NEXUS_DEBUG("LargeData") << "✓ Cleaned orphaned channel: " << name
                                         << " (" << (shm_size / 1024 / 1024) << " MB)";
            } else {
                NEXUS_ERROR("LargeData") << "✗ Failed to unlink " << name << ": " << strerror(errno);
            }

            // 解锁（虽然close会自动解锁，但显式调用是个好习惯）
            flock(fd, LOCK_UN);
        } else {
            // EWOULDBLOCK 说明有人在使用
            // NEXUS_DEBUG("LargeData") << "Channel in use: " << name;
        }

        close(fd);
    }

    closedir(dir);

    if (cleaned_count > 0) {
        NEXUS_DEBUG("LargeData") << "Cleanup complete: removed " << cleaned_count << " channel(s), freed "
                                 << (total_freed / 1024 / 1024) << " MB";
    } else {
        NEXUS_DEBUG("LargeData") << "No orphaned channels found";
    }

    return cleaned_count;
}

// ============ SPMC辅助方法 ============

// 注册读者，返回reader_id（-1表示失败）
int32_t LargeDataChannel::registerReader() {
    // 查找空闲槽位
    for (size_t i = 0; i < MAX_READERS; ++i) {
        bool expected = false;
        if (control_->readers[i].active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // 成功占用槽位，初始化
            // 🔧 策略：从0开始读取，但在tryRead时会检查并跳过已被覆盖的数据
            // 这样可以读取注册前已写入的所有数据（如果还在缓冲区中）
            control_->readers[i].read_pos.store(0, std::memory_order_release);
            control_->readers[i].pid.store(getpid(), std::memory_order_release);
            control_->readers[i].heartbeat.store(static_cast<uint64_t>(time(nullptr)), std::memory_order_release);

            // 递增读者计数
            uint32_t new_count = control_->num_readers.fetch_add(1, std::memory_order_acq_rel) + 1;

            NEXUS_DEBUG("LargeData") << "Registered as reader #" << i << ", total readers: " << new_count
                                     << ", PID: " << getpid();

            return static_cast<int32_t>(i);
        }
    }

    NEXUS_ERROR("LargeData") << "Failed to register reader: max readers (" << MAX_READERS << ") exceeded";
    return -1;
}

// 注销读者
void LargeDataChannel::unregisterReader(int32_t reader_id) {
    if (reader_id < 0 || reader_id >= static_cast<int32_t>(MAX_READERS)) {
        return;
    }

    // 标记为不活跃
    control_->readers[reader_id].active.store(false, std::memory_order_release);
    control_->readers[reader_id].pid.store(0, std::memory_order_release);

    // 递减读者计数
    uint32_t prev_count = control_->num_readers.fetch_sub(1, std::memory_order_acq_rel);

    NEXUS_DEBUG("LargeData") << "Unregistered reader #" << reader_id << ", total readers: " << prev_count << " -> "
                             << (prev_count - 1);
}

// 获取所有读者中的最小read_pos（用于垃圾回收）
uint64_t LargeDataChannel::getMinReadPos() const {
    uint64_t min_pos = control_->write_pos.load(std::memory_order_acquire);

    for (size_t i = 0; i < MAX_READERS; ++i) {
        if (control_->readers[i].active.load(std::memory_order_acquire)) {
            uint64_t pos = control_->readers[i].read_pos.load(std::memory_order_acquire);
            if (pos < min_pos) {
                min_pos = pos;
            }
        }
    }

    return min_pos;
}

// 更新读者心跳
void LargeDataChannel::updateReaderHeartbeat(int32_t reader_id) {
    if (reader_id < 0 || reader_id >= static_cast<int32_t>(MAX_READERS)) {
        return;
    }

    uint64_t current_time = static_cast<uint64_t>(time(nullptr));
    control_->readers[reader_id].heartbeat.store(current_time, std::memory_order_relaxed);
}

// 清理死亡的读者
void LargeDataChannel::cleanupDeadReaders() {
    uint64_t current_time = static_cast<uint64_t>(time(nullptr));
    static uint64_t last_cleanup = 0;

    // 定期清理（每30秒）
    if (current_time - last_cleanup < LARGE_DATA_CLEANUP_INTERVAL_S) {
        return;
    }
    last_cleanup = current_time;

    // 辅助函数：检查进程是否存活
    auto isProcessAlive = [](int32_t pid) -> bool {
        if (pid <= 0) {
            return false;
        }
        if (kill(pid, 0) == 0) {
            return true;
        }
        return errno != ESRCH;
    };

    for (size_t i = 0; i < MAX_READERS; ++i) {
        if (!control_->readers[i].active.load(std::memory_order_acquire)) {
            continue;
        }

        int32_t pid = control_->readers[i].pid.load(std::memory_order_relaxed);
        uint64_t hb = control_->readers[i].heartbeat.load(std::memory_order_relaxed);

        // 检查1：进程是否存活
        bool process_dead = !isProcessAlive(pid);

        // 检查2：心跳是否超时
        bool heartbeat_timeout = (current_time - hb) > LARGE_DATA_READER_TIMEOUT_S;

        if (process_dead || heartbeat_timeout) {
            NEXUS_DEBUG("LargeData") << "Cleaning dead reader #" << i << ", PID: " << pid << ", dead: " << process_dead
                                     << ", timeout: " << heartbeat_timeout;

            // 标记为不活跃并递减计数
            control_->readers[i].active.store(false, std::memory_order_release);
            control_->num_readers.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
}

}  // namespace rpc
}  // namespace Nexus
