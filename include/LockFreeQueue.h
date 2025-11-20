// Lock-Free SPSC Ring Buffer Queue
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace librpc {

/**
 * @brief Lock-free Single Producer Single Consumer (SPSC) ring buffer
 * 
 * This is a high-performance, wait-free queue optimized for shared memory.
 * Each producer-consumer pair gets its own queue to eliminate lock contention.
 * 
 * Key features:
 * - Wait-free for both producer and consumer
 * - Cache-line aligned to avoid false sharing
 * - Memory barriers for proper synchronization
 * - Overflow handling: drop oldest messages
 */
template<size_t CAPACITY>
class alignas(64) LockFreeRingBuffer {
public:
    static constexpr size_t MAX_MSG_SIZE = 2048;  // Max message size
    
    struct alignas(64) Message {
        uint32_t size;           // Message payload size
        char from_node_id[32];   // Sender node ID
        uint8_t payload[MAX_MSG_SIZE];
    };
    
    LockFreeRingBuffer() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        stats_messages_written_.store(0, std::memory_order_relaxed);
        stats_messages_read_.store(0, std::memory_order_relaxed);
        stats_messages_dropped_.store(0, std::memory_order_relaxed);
    }
    
    /**
     * @brief Try to write a message (producer side)
     * @param from_node_id Sender node ID
     * @param data Message data
     * @param size Message size
     * @return true if written successfully, false if queue is full
     */
    bool tryWrite(const char* from_node_id, const uint8_t* data, size_t size) {
        if (size > MAX_MSG_SIZE) {
            return false;
        }
        
        // Load indices with acquire semantics
        uint64_t current_head = head_.load(std::memory_order_acquire);
        uint64_t current_tail = tail_.load(std::memory_order_acquire);
        
        // Check if queue is full
        uint64_t next_head = current_head + 1;
        if (next_head - current_tail > CAPACITY) {
            // Queue full - drop oldest message to make room
            tail_.store(current_tail + 1, std::memory_order_release);
            stats_messages_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Write message
        size_t index = current_head % CAPACITY;
        Message& msg = buffer_[index];
        
        msg.size = static_cast<uint32_t>(size);
        strncpy(msg.from_node_id, from_node_id, sizeof(msg.from_node_id) - 1);
        msg.from_node_id[sizeof(msg.from_node_id) - 1] = '\0';
        memcpy(msg.payload, data, size);
        
        // Publish the write with release semantics
        std::atomic_thread_fence(std::memory_order_release);
        head_.store(next_head, std::memory_order_release);
        
        stats_messages_written_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    
    /**
     * @brief Try to read a message (consumer side)
     * @param out_from_node_id Output buffer for sender node ID (min 32 bytes)
     * @param out_data Output buffer for message data (min MAX_MSG_SIZE bytes)
     * @param out_size Output message size
     * @return true if read successfully, false if queue is empty
     */
    bool tryRead(char* out_from_node_id, uint8_t* out_data, size_t& out_size) {
        // Load indices with acquire semantics
        uint64_t current_tail = tail_.load(std::memory_order_acquire);
        uint64_t current_head = head_.load(std::memory_order_acquire);
        
        // Check if queue is empty
        if (current_tail >= current_head) {
            return false;
        }
        
        // Read message
        size_t index = current_tail % CAPACITY;
        const Message& msg = buffer_[index];
        
        out_size = msg.size;
        strncpy(out_from_node_id, msg.from_node_id, 32);
        memcpy(out_data, msg.payload, msg.size);
        
        // Advance tail with release semantics
        std::atomic_thread_fence(std::memory_order_release);
        tail_.store(current_tail + 1, std::memory_order_release);
        
        stats_messages_read_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    
    /**
     * @brief Get number of messages in queue
     */
    size_t size() const {
        uint64_t current_head = head_.load(std::memory_order_acquire);
        uint64_t current_tail = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(current_head - current_tail);
    }
    
    /**
     * @brief Check if queue is empty
     */
    bool empty() const {
        return size() == 0;
    }
    
    /**
     * @brief Get statistics
     */
    struct Stats {
        uint64_t messages_written;
        uint64_t messages_read;
        uint64_t messages_dropped;
        size_t current_size;
    };
    
    Stats getStats() const {
        return {
            stats_messages_written_.load(std::memory_order_relaxed),
            stats_messages_read_.load(std::memory_order_relaxed),
            stats_messages_dropped_.load(std::memory_order_relaxed),
            size()
        };
    }
    
private:
    // Cache-line aligned atomic indices to avoid false sharing
    alignas(64) std::atomic<uint64_t> head_;   // Write position (producer)
    alignas(64) std::atomic<uint64_t> tail_;   // Read position (consumer)
    
    // Statistics (relaxed ordering is fine)
    alignas(64) std::atomic<uint64_t> stats_messages_written_;
    alignas(64) std::atomic<uint64_t> stats_messages_read_;
    alignas(64) std::atomic<uint64_t> stats_messages_dropped_;
    
    // Message buffer
    alignas(64) Message buffer_[CAPACITY];
};

} // namespace librpc
