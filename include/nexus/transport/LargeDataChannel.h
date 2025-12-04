// 大数据专用通道 - 用于频繁传输大数据场景
// 
// 设计原理：
// 1. 独立的共享内存环形缓冲区（避免影响小消息队列）
// 2. 零拷贝设计（直接在共享内存中分配和读取）
// 3. 通过V3消息队列发送通知（携带偏移量和大小）
// 4. 支持多生产者-多消费者（MPMC）

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <atomic>
#include <memory>
#include <functional>

namespace Nexus {
namespace rpc {

// 大数据块头部
struct LargeDataHeader {
    uint32_t magic;          // 魔数：0x4C444154 ('LDAT')
    uint32_t size;           // 数据大小
    uint64_t sequence;       // 序列号
    uint32_t crc32;          // CRC32校验
    char topic[64];          // Topic名称
    uint8_t reserved[44];    // 保留字段（对齐到128字节）
    
    static constexpr uint32_t MAGIC = 0x4C444154;
} __attribute__((packed));

static_assert(sizeof(LargeDataHeader) == 128, "LargeDataHeader must be 128 bytes");

// 最大读者数量（可在Config中配置）
static constexpr size_t MAX_READERS = 16;

// 单个读者槽位（Cache line对齐，避免false sharing）
struct ReaderSlot {
    std::atomic<uint64_t> read_pos;      // 读取位置（字节偏移） - 8B
    std::atomic<uint64_t> heartbeat;     // 心跳时间戳（秒） - 8B
    std::atomic<int32_t> pid;            // 进程ID - 4B
    std::atomic<bool> active;            // 是否活跃 - 1B
    char padding[43];                    // 填充到64字节 - 43B
    
    ReaderSlot() : read_pos(0), heartbeat(0), pid(0), active(false) {
        memset(padding, 0, sizeof(padding));
    }
} __attribute__((aligned(64)));

static_assert(sizeof(ReaderSlot) == 64, "ReaderSlot must be 64 bytes (1 cache line)");

// 环形缓冲区控制块（SPMC设计）
struct RingBufferControl {
    // 写端专用（Cache line对齐）
    std::atomic<uint64_t> write_pos;        // 写入位置（字节偏移） - 8B
    std::atomic<uint64_t> sequence;         // 当前序列号 - 8B
    std::atomic<uint64_t> writer_heartbeat; // 写端心跳时间戳（秒） - 8B
    std::atomic<int32_t> writer_pid;        // 写端进程ID - 4B
    char padding1[36];                      // 填充到64字节 - 36B
    
    // 读者数组（每个读者独立的Cache line）
    ReaderSlot readers[MAX_READERS];        // 16 × 64B = 1024B
    
    // 共享元数据（Cache line对齐）
    std::atomic<uint32_t> num_readers;      // 当前读者数量 - 4B
    std::atomic<int32_t> ref_count;         // 引用计数 - 4B
    uint64_t capacity;                      // 缓冲区容量 - 8B
    uint32_t max_block_size;                // 单个数据块最大大小 - 4B
    uint32_t max_readers;                   // 最大读者数量 - 4B
    char padding2[40];                      // 填充到64字节 - 40B
    
    RingBufferControl() 
        : write_pos(0), sequence(0), writer_heartbeat(0), writer_pid(0),
          num_readers(0), ref_count(0),
          capacity(0), max_block_size(0), max_readers(MAX_READERS) {
        memset(padding1, 0, sizeof(padding1));
        memset(padding2, 0, sizeof(padding2));
    }
} __attribute__((aligned(64)));

// 大数据通道溢出策略
enum class LargeDataOverflowPolicy {
    DROP_OLDEST,    // 丢弃最老的数据块（默认，适合视频流）
    DROP_NEWEST,    // 丢弃最新的数据块（适合传感器数据）
    BLOCK           // 阻塞直到有空间（可能导致死锁，慎用）
};

// 大数据通道溢出回调
// @param channel_name 通道名称
// @param topic 主题名称
// @param dropped_sequence 被丢弃的数据块序列号
// @param total_dropped 总丢弃数量
using LargeDataOverflowCallback = std::function<void(const std::string& channel_name,
                                                       const std::string& topic,
                                                       uint64_t dropped_sequence,
                                                       size_t total_dropped)>;

// 大数据通道
class LargeDataChannel {
public:
    // 配置参数
    struct Config {
        size_t buffer_size;      // 环形缓冲区大小（默认64MB）
        size_t max_block_size;   // 单个数据块最大大小（默认8MB）
        size_t max_readers;      // 最大读者数量（默认8）
        bool use_mmap_noreserve; // 使用MAP_NORESERVE优化
        LargeDataOverflowPolicy overflow_policy;  // 溢出策略（默认DROP_OLDEST）
        LargeDataOverflowCallback overflow_callback;  // 溢出回调
        
        Config() 
            : buffer_size(64 * 1024 * 1024),
              max_block_size(8 * 1024 * 1024),
              max_readers(8),
              use_mmap_noreserve(true),
              overflow_policy(LargeDataOverflowPolicy::DROP_OLDEST),
              overflow_callback(nullptr) {}
    };
    
    // 读取结果枚举
    enum class ReadResult {
        SUCCESS,         // 成功读取数据
        NO_DATA,         // 缓冲区为空
        CRC_ERROR,       // CRC校验失败
        INVALID_MAGIC,   // Magic不匹配（可能数据未写完）
        SIZE_EXCEEDED,   // 数据大小超限
        INSUFFICIENT     // 数据不完整
    };
    
    // 数据块句柄（零拷贝访问）
    struct DataBlock {
        const LargeDataHeader* header;
        const uint8_t* data;
        size_t size;
        ReadResult result;  // 读取结果
        
        DataBlock() : header(nullptr), data(nullptr), size(0), result(ReadResult::NO_DATA) {}
        
        bool isValid() const { 
            return result == ReadResult::SUCCESS && 
                   header && header->magic == LargeDataHeader::MAGIC; 
        }
        
        std::string getTopic() const {
            return header ? std::string(header->topic) : "";
        }
        
        const char* getResultString() const {
            switch (result) {
                case ReadResult::SUCCESS: return "SUCCESS";
                case ReadResult::NO_DATA: return "NO_DATA";
                case ReadResult::CRC_ERROR: return "CRC_ERROR";
                case ReadResult::INVALID_MAGIC: return "INVALID_MAGIC";
                case ReadResult::SIZE_EXCEEDED: return "SIZE_EXCEEDED";
                case ReadResult::INSUFFICIENT: return "INSUFFICIENT";
                default: return "UNKNOWN";
            }
        }
    };
    
    // 创建或连接到大数据通道
    static std::shared_ptr<LargeDataChannel> create(
        const std::string& shm_name, 
        const Config& config = Config());
    
    ~LargeDataChannel();
    
    // 写入大数据（返回序列号，-1表示失败）
    // 注意：data会被直接写入共享内存，调用者可以在返回后释放原始data
    int64_t write(const std::string& topic, 
                  const uint8_t* data, 
                  size_t size);
    
    // 尝试读取一个数据块（非阻塞）
    // 返回的DataBlock指向共享内存，读取完后必须调用releaseBlock
    bool tryRead(DataBlock& block);
    
    // 释放已读取的数据块（更新read_pos）
    void releaseBlock(const DataBlock& block);
    
    // 获取统计信息
    struct Stats {
        uint64_t total_writes;
        uint64_t total_reads;
        uint64_t total_bytes_written;
        uint64_t total_bytes_read;
        uint64_t current_usage;  // 当前占用字节数
        uint64_t capacity;
        
        double usage_percent() const {
            return capacity > 0 ? (static_cast<double>(current_usage) * 100.0 / static_cast<double>(capacity)) : 0.0;
        }
    };
    
    Stats getStats() const;
    
    // 获取可用空间
    size_t getAvailableSpace() const;
    
    // 检查是否可以写入指定大小
    bool canWrite(size_t size) const;
    
    // 设置溢出策略（动态修改）
    void setOverflowPolicy(LargeDataOverflowPolicy policy);
    
    // 设置溢出回调（动态修改）
    void setOverflowCallback(LargeDataOverflowCallback callback);
    
    // 清理过期的大数据通道（静态工具函数）
    // 在程序启动时调用，清理上次异常退出遗留的共享内存
    // @param timeout_seconds 心跳超时时间（默认60秒）
    // @return 清理的通道数量
    static size_t cleanupOrphanedChannels(uint32_t timeout_seconds = 60);
    
private:
    LargeDataChannel(const std::string& shm_name, const Config& config);
    
    bool initialize();
    ReadResult validateBlock(const LargeDataHeader* header, size_t available) const;
    uint32_t calculateCRC32(const uint8_t* data, size_t size) const;
    
    // SPMC相关私有方法
    int32_t registerReader();     // 注册读者，返回reader_id（-1表示失败）
    void unregisterReader(int32_t reader_id);  // 注销读者
    uint64_t getMinReadPos() const;  // 获取所有读者中的最小read_pos（用于垃圾回收）
    void updateReaderHeartbeat(int32_t reader_id);  // 更新读者心跳
    void cleanupDeadReaders();    // 清理死亡的读者
    
    std::string shm_name_;
    Config config_;
    int shm_fd_;
    void* shm_addr_;
    size_t shm_size_;
    
    RingBufferControl* control_;
    uint8_t* buffer_;
    
    // 当前读者ID（每个LargeDataChannel实例）
    int32_t reader_id_;
    
    // 统计信息
    mutable std::atomic<uint64_t> total_writes_;
    mutable std::atomic<uint64_t> total_reads_;
    mutable std::atomic<uint64_t> total_bytes_written_;
    mutable std::atomic<uint64_t> total_bytes_read_;
    mutable std::atomic<uint64_t> total_dropped_;  // 总丢弃数量
};

// 通知消息（通过V3队列发送）
struct LargeDataNotification {
    uint64_t sequence;       // 8B - 对应LargeDataChannel中的序列号
    uint32_t size;           // 4B - 数据大小
    uint32_t reserved1;      // 4B - 对齐
    char channel_name[32];   // 32B - 通道名称
    char topic[64];          // 64B - Topic
    uint8_t reserved2[16];   // 16B - 保留
} __attribute__((packed));

static_assert(sizeof(LargeDataNotification) == 128, 
              "LargeDataNotification must be 128 bytes");

} // namespace rpc
} // namespace Nexus
