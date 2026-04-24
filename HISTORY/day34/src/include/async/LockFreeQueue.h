#pragma once
/**
 * @file LockFreeQueue.h
 * @brief 无锁队列实现
 *
 * 提供两种无锁队列：
 * 1. MPMCQueue - 多生产者多消费者，基于 Michael & Scott 算法
 * 2. SPSCQueue - 单生产者单消费者，环形缓冲区，性能最优
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

namespace mcpp::async {

// ─────────────────────────────────────────────────────────────────
//  缓存行对齐辅助
// ─────────────────────────────────────────────────────────────────

#ifdef __cpp_lib_hardware_interference_size
constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLineSize = 64;
#endif

// ─────────────────────────────────────────────────────────────────
//  SPSC 无锁队列 (单生产者单消费者)
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 单生产者单消费者无锁环形队列
 *
 * 性能特征：
 * - 入队/出队 O(1)，无锁
 * - 容量必须是 2 的幂（用位运算优化取模）
 * - 适用于：worker 线程内部队列
 *
 * @tparam T 元素类型，要求可移动构造
 */
template <typename T> class SPSCQueue {
  public:
    /**
     * @brief 构造函数
     * @param capacity 容量（向上取整到 2 的幂）
     */
    explicit SPSCQueue(std::size_t capacity = 1024)
        : capacity_(roundUpToPowerOf2(capacity)), mask_(capacity_ - 1),
          buffer_(new Slot[capacity_]) {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    ~SPSCQueue() {
        // 析构剩余元素
        T item;
        while (tryPop(item)) {
        }
        delete[] buffer_;
    }

    SPSCQueue(const SPSCQueue &) = delete;
    SPSCQueue &operator=(const SPSCQueue &) = delete;

    /**
     * @brief 入队 (生产者调用)
     * @return true 成功，false 队列满
     */
    bool tryPush(T item) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = head + 1;

        // 检查队列是否满
        if (next - tail_.load(std::memory_order_acquire) > capacity_) {
            return false;
        }

        new (&buffer_[head & mask_].data) T(std::move(item));
        head_.store(next, std::memory_order_release);
        return true;
    }

    /**
     * @brief 出队 (消费者调用)
     * @return true 成功，false 队列空
     */
    bool tryPop(T &item) {
        const auto tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // 空
        }

        T *ptr = reinterpret_cast<T *>(&buffer_[tail & mask_].data);
        item = std::move(*ptr);
        ptr->~T();

        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief 当前大小（近似值）
     */
    std::size_t size() const noexcept {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    bool empty() const noexcept { return size() == 0; }
    std::size_t capacity() const noexcept { return capacity_; }

  private:
    struct alignas(kCacheLineSize) Slot {
        std::aligned_storage_t<sizeof(T), alignof(T)> data;
    };

    static std::size_t roundUpToPowerOf2(std::size_t n) {
        if (n == 0)
            return 1;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    Slot *buffer_;

    // 缓存行隔离，避免 false sharing
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;
};

// ─────────────────────────────────────────────────────────────────
//  MPMC 无锁队列 (多生产者多消费者)
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 多生产者多消费者无锁队列
 *
 * 基于 Vyukov MPMC bounded queue 算法。
 *
 * 性能特征：
 * - 入队/出队 O(1) 期望
 * - 容量必须是 2 的幂
 * - 适用于：work-stealing 全局任务队列
 *
 * @tparam T 元素类型
 */
template <typename T> class MPMCQueue {
  public:
    explicit MPMCQueue(std::size_t capacity = 1024)
        : capacity_(roundUpToPowerOf2(capacity)), mask_(capacity_ - 1),
          buffer_(new Cell[capacity_]) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
    }

    ~MPMCQueue() {
        T item;
        while (tryPop(item)) {
        }
        delete[] buffer_;
    }

    MPMCQueue(const MPMCQueue &) = delete;
    MPMCQueue &operator=(const MPMCQueue &) = delete;

    bool tryPush(T item) {
        Cell *cell;
        std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // 满
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }

        new (&cell->data) T(std::move(item));
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool tryPop(T &item) {
        Cell *cell;
        std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // 空
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);
            }
        }

        T *ptr = reinterpret_cast<T *>(&cell->data);
        item = std::move(*ptr);
        ptr->~T();
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    /**
     * @brief 近似大小
     */
    std::size_t size() const noexcept {
        auto enq = enqueuePos_.load(std::memory_order_acquire);
        auto deq = dequeuePos_.load(std::memory_order_acquire);
        return enq >= deq ? enq - deq : 0;
    }

    bool empty() const noexcept { return size() == 0; }

  private:
    struct alignas(kCacheLineSize) Cell {
        std::atomic<std::size_t> sequence;
        std::aligned_storage_t<sizeof(T), alignof(T)> data;
    };

    static std::size_t roundUpToPowerOf2(std::size_t n) {
        if (n == 0)
            return 1;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    Cell *buffer_;

    alignas(kCacheLineSize) std::atomic<std::size_t> enqueuePos_;
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeuePos_;
};

} // namespace mcpp::async
