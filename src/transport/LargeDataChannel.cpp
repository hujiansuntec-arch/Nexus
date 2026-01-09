// Copyright (c) 2025 Baidu.com, Inc. All Rights Reserved
//
// LargeDataChannel 实现 - 高频大数据传输专用通道

#include "nexus/transport/LargeDataChannel.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>  // 用于 kill() 检测进程存活
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

// CRC32查找表
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3, 0x0edb8832,
    0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 0x136c9856, 0x646ba8c0, 0xfd62f97a,
    0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3,
    0x45df5c75, 0xdcd60dcf, 0xabd13d59, 0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab,
    0xb6662d3d, 0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01, 0x6b6b51f4,
    0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65, 0x4db26158, 0x3ab551ce, 0xa3bc0074,
    0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525,
    0x206f85b3, 0xb966d409, 0xce61e49f, 0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615,
    0x73dc1683, 0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7, 0xfed41b76,
    0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b, 0xd80d2bda, 0xaf0a1b4c, 0x36034af6,
    0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7,
    0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d, 0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7,
    0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45, 0xa00ae278,
    0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9, 0xbdbdf21c, 0xcabac28a, 0x53b39330,
    0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d};

// 计算CRC32
uint32_t LargeDataChannel::calculateCRC32(const uint8_t* data, size_t size) const {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
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

// 写入大数据
int64_t LargeDataChannel::write(const std::string& topic, const uint8_t* data, size_t size) {
    if (size > config_.max_block_size) {
        NEXUS_ERROR("LargeData") << "Data size " << size << " exceeds max block size " << config_.max_block_size;
        return -1;
    }

    // 总大小 = 头部 + 数据
    size_t total_size = sizeof(LargeDataHeader) + size;

    // 清理死亡的读者（定期执行）
    cleanupDeadReaders();

    // 检查可用空间（基于所有读者中的最小read_pos）
    uint64_t min_read_pos = getMinReadPos();
    uint64_t write_pos = control_->write_pos.load(std::memory_order_acquire);
    uint64_t used = write_pos - min_read_pos;

    // 缓冲区满时的处理策略
    if (used + total_size > control_->capacity) {
        switch (config_.overflow_policy) {
            case LargeDataOverflowPolicy::DROP_OLDEST:
                // 不实现：SPMC模式下无法安全丢弃（会影响所有读者）
                // 返回失败，由上层决定是否重试
                NEXUS_ERROR("LargeData") << "Buffer full (DROP_OLDEST not supported in SPMC mode)";
                break;

            case LargeDataOverflowPolicy::DROP_NEWEST:
                // 丢弃当前写入
                {
                    size_t dropped = total_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
                    NEXUS_ERROR("LargeData") << "Buffer full, dropping newest data (total: " << dropped << ")";

                    // 调用溢出回调
                    if (config_.overflow_callback) {
                        try {
                            uint64_t seq = control_->sequence.load(std::memory_order_relaxed);
                            config_.overflow_callback(shm_name_, topic, seq, dropped);
                        } catch (...) {
                            // 忽略回调异常
                        }
                    }
                }
                return -1;

            case LargeDataOverflowPolicy::BLOCK:
                // 阻塞等待（不推荐，可能死锁）
                NEXUS_ERROR("LargeData") << "Buffer full (BLOCK policy not recommended)";
                break;
        }

        return -1;
    }

    // 更新写端心跳
    uint64_t current_time = static_cast<uint64_t>(time(nullptr));
    control_->writer_heartbeat.store(current_time, std::memory_order_relaxed);

    // 获取序列号
    uint64_t seq = control_->sequence.fetch_add(1);

    // 计算写入位置（环形缓冲区）
    uint64_t write_offset = write_pos % control_->capacity;

    // 检查是否需要环绕
    if (write_offset + total_size > control_->capacity) {
        // 环绕到开头（浪费剩余空间）
        uint64_t skip_size = control_->capacity - write_offset;
        control_->write_pos.fetch_add(skip_size);
        write_pos = control_->write_pos.load();
        write_offset = 0;

        // 🔧 关键修复：通知所有读者跳过浪费的空间
        for (size_t i = 0; i < MAX_READERS; ++i) {
            if (control_->readers[i].active.load(std::memory_order_acquire)) {
                uint64_t reader_pos = control_->readers[i].read_pos.load(std::memory_order_acquire);
                // 如果读者还在旧的环内，需要跳过浪费的空间
                if (reader_pos < write_pos && (reader_pos % control_->capacity) >= write_offset) {
                    control_->readers[i].read_pos.store(write_pos, std::memory_order_release);
                    NEXUS_WARN("LargeData")
                        << "Reader #" << i << " skipped " << skip_size << " bytes due to ring wrap (from " << reader_pos
                        << " to " << write_pos << ")";
                }
            }
        }
    }

    // 准备头部（先不写magic，避免读端读到未完成数据）
    LargeDataHeader* header = reinterpret_cast<LargeDataHeader*>(buffer_ + write_offset);
    header->magic = 0;  // 暂时设为0，最后才写入作为"完成标志"
    header->size = static_cast<uint32_t>(size);
    header->sequence = seq;
    strncpy(header->topic, topic.c_str(), sizeof(header->topic) - 1);
    header->topic[sizeof(header->topic) - 1] = '\0';

    // 写入数据
    uint8_t* data_ptr = buffer_ + write_offset + sizeof(LargeDataHeader);
    memcpy(data_ptr, data, size);

    // 计算CRC32
    header->crc32 = calculateCRC32(data, size);

    // 内存屏障：确保所有数据写入对其他进程可见
    std::atomic_thread_fence(std::memory_order_release);

    // 最后写入magic作为"发布"标志（发布-订阅模式）
    // 必须使用atomic store确保跨进程可见性
    reinterpret_cast<std::atomic<uint32_t>*>(&header->magic)->store(LargeDataHeader::MAGIC, std::memory_order_release);

    // 更新写指针（使用release语义）
    control_->write_pos.fetch_add(total_size, std::memory_order_release);

    // 更新统计
    total_writes_.fetch_add(1);
    total_bytes_written_.fetch_add(size);

    return seq;
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
            control_->readers[reader_id_].read_pos.fetch_add(sizeof(LargeDataHeader), std::memory_order_release);
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
    control_->readers[reader_id_].read_pos.fetch_add(total_size, std::memory_order_release);

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
    const uint8_t* data = reinterpret_cast<const uint8_t*>(header) + sizeof(LargeDataHeader);
    uint32_t calculated_crc = calculateCRC32(data, header->size);

    if (calculated_crc != header->crc32) {
        NEXUS_ERROR("LargeData") << "CRC32 mismatch: expected " << header->crc32 << ", got " << calculated_crc;
        return ReadResult::CRC_ERROR;
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
    return getAvailableSpace() >= size;
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
size_t LargeDataChannel::cleanupOrphanedChannels(uint32_t timeout_seconds) {
    NEXUS_DEBUG("LargeData") << "Scanning for orphaned channels (timeout: " << timeout_seconds << "s)...";

    DIR* dir = opendir("/dev/shm");
    if (!dir) {
        NEXUS_ERROR("LargeData") << "Failed to open /dev/shm: " << strerror(errno);
        return 0;
    }

    struct dirent* entry;
    size_t cleaned_count = 0;
    size_t total_freed = 0;
    time_t current_time = time(nullptr);

    // 辅助函数：检查进程是否存活
    auto isProcessAlive = [](int32_t pid) -> bool {
        if (pid <= 0) {
            return false;  // 无效PID
        }

        // kill(pid, 0) 不发送信号，只检查进程是否存在
        if (kill(pid, 0) == 0) {
            return true;  // 进程存在且有权限访问
        }

        // ESRCH: 进程不存在
        // EPERM: 进程存在但无权限（也算存活）
        return errno != ESRCH;
    };

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

        // 获取文件大小
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            continue;
        }

        size_t shm_size = st.st_size;

        // 映射共享内存以检查控制结构
        void* addr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            continue;
        }

        bool should_cleanup = false;
        std::string cleanup_reason;

        // 检查是否为有效的LargeDataChannel
        if (shm_size >= sizeof(RingBufferControl) + LARGE_DATA_MIN_VALID_SIZE) {
            RingBufferControl* control = static_cast<RingBufferControl*>(addr);

            // **优先级1: 引用计数检测**
            int32_t ref_count = control->ref_count.load(std::memory_order_relaxed);
            if (ref_count == 0) {
                should_cleanup = true;
                cleanup_reason = "zero ref_count";
            }

            // **优先级2: 所有进程死亡检测**
            if (!should_cleanup) {
                int32_t writer_pid = control->writer_pid.load(std::memory_order_relaxed);
                bool writer_alive = isProcessAlive(writer_pid);

                // 检查所有读者是否存活
                bool any_reader_alive = false;
                for (size_t i = 0; i < MAX_READERS; ++i) {
                    if (control->readers[i].active.load(std::memory_order_relaxed)) {
                        int32_t reader_pid = control->readers[i].pid.load(std::memory_order_relaxed);
                        if (isProcessAlive(reader_pid)) {
                            any_reader_alive = true;
                            break;
                        }
                    }
                }

                // 如果写端和所有读者都死亡，清理
                if (!writer_alive && !any_reader_alive && (writer_pid > 0 || control->num_readers.load() > 0)) {
                    should_cleanup = true;
                    cleanup_reason = "all processes dead (writer PID: " + std::to_string(writer_pid) + ")";
                }
            }

            // **优先级3: 心跳超时检测（fallback）**
            if (!should_cleanup) {
                uint64_t writer_hb = control->writer_heartbeat.load(std::memory_order_relaxed);
                bool writer_timeout = (writer_hb > 0 && (current_time - writer_hb) > timeout_seconds);

                // 检查所有读者心跳
                bool all_readers_timeout = true;
                uint32_t num_readers = control->num_readers.load(std::memory_order_relaxed);
                if (num_readers > 0) {
                    for (size_t i = 0; i < MAX_READERS; ++i) {
                        if (control->readers[i].active.load(std::memory_order_relaxed)) {
                            uint64_t hb = control->readers[i].heartbeat.load(std::memory_order_relaxed);
                            if (hb == 0 || (current_time - hb) <= timeout_seconds) {
                                all_readers_timeout = false;
                                break;
                            }
                        }
                    }
                }

                if (writer_timeout && (num_readers == 0 || all_readers_timeout)) {
                    should_cleanup = true;
                    cleanup_reason = "all heartbeats timeout";
                }
            }
        } else {
            // 太小，不是有效的LargeDataChannel
            if ((current_time - st.st_mtime) > timeout_seconds) {
                should_cleanup = true;
                cleanup_reason = "invalid size and old";
            }
        }

        // 清理
        munmap(addr, shm_size);
        close(fd);

        if (should_cleanup) {
            if (shm_unlink(name.c_str()) == 0) {
                cleaned_count++;
                total_freed += shm_size;
                NEXUS_DEBUG("LargeData") << "✓ Cleaned: " << name << " (" << (shm_size / 1024 / 1024) << " MB) - "
                                         << cleanup_reason;
            } else {
                NEXUS_ERROR("LargeData") << "✗ Failed to unlink " << name << ": " << strerror(errno);
            }
        }
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
