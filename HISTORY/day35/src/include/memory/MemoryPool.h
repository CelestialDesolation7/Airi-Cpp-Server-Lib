#pragma once
/**
 * @file MemoryPool.h
 * @brief 内存池实现
 *
 * 提供两种内存池：
 * 1. FixedSizePool - 固定大小对象池（适用于同类型对象高频分配）
 * 2. SlabAllocator - 多 slab 分级分配器（适用于多种小对象）
 *
 * 设计目标：
 * - 减少 malloc/free 开销（10-100x 加速）
 * - 减少内存碎片
 * - 提升缓存局部性
 *
 * 参考：Linux SLUB allocator、jemalloc/tcmalloc
 */

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace mcpp::memory {

// ─────────────────────────────────────────────────────────────────
//  固定大小对象池
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 固定大小对象内存池
 *
 * 预分配一大块内存，按对象大小切分，使用 free-list 管理空闲块。
 *
 * 性能特征：
 * - allocate/deallocate O(1)
 * - 线程不安全（需要外部加锁或使用 ThreadLocalPool）
 * - 适用于：Connection 对象、HttpContext 等高频分配的对象
 *
 * @tparam T 对象类型
 * @tparam BlockSize 每个 block 包含的对象数量
 */
template <typename T, std::size_t BlockSize = 1024> class FixedSizePool {
  public:
    FixedSizePool() : freeList_(nullptr), totalAllocated_(0), totalFreed_(0) {}

    ~FixedSizePool() {
        // 释放所有 block
        for (auto *block : blocks_) {
            ::operator delete(block);
        }
    }

    FixedSizePool(const FixedSizePool &) = delete;
    FixedSizePool &operator=(const FixedSizePool &) = delete;

    /**
     * @brief 分配一个对象的内存（不构造）
     */
    T *allocate() {
        if (!freeList_) {
            allocateBlock();
        }

        FreeNode *node = freeList_;
        freeList_ = node->next;
        ++totalAllocated_;
        return reinterpret_cast<T *>(node);
    }

    /**
     * @brief 释放一个对象的内存（不析构）
     */
    void deallocate(T *ptr) noexcept {
        if (!ptr)
            return;

        FreeNode *node = reinterpret_cast<FreeNode *>(ptr);
        node->next = freeList_;
        freeList_ = node;
        ++totalFreed_;
    }

    /**
     * @brief 构造一个对象
     */
    template <typename... Args> T *construct(Args &&...args) {
        T *ptr = allocate();
        new (ptr) T(std::forward<Args>(args)...);
        return ptr;
    }

    /**
     * @brief 析构并释放对象
     */
    void destroy(T *ptr) noexcept {
        if (!ptr)
            return;
        ptr->~T();
        deallocate(ptr);
    }

    /**
     * @brief 统计信息
     */
    struct Stats {
        std::size_t blocksAllocated;
        std::size_t totalCapacity;
        std::size_t allocations;
        std::size_t deallocations;
        std::size_t inUse;
    };

    Stats stats() const noexcept {
        return Stats{blocks_.size(), blocks_.size() * BlockSize, totalAllocated_, totalFreed_,
                     totalAllocated_ - totalFreed_};
    }

  private:
    union FreeNode {
        FreeNode *next;
        std::aligned_storage_t<sizeof(T), alignof(T)> storage;
    };

    void allocateBlock() {
        // 一次分配一个 block (BlockSize 个对象)
        FreeNode *block =
            reinterpret_cast<FreeNode *>(::operator new(sizeof(FreeNode) * BlockSize));
        blocks_.push_back(block);

        // 链接 free list
        for (std::size_t i = 0; i < BlockSize - 1; ++i) {
            block[i].next = &block[i + 1];
        }
        block[BlockSize - 1].next = freeList_;
        freeList_ = &block[0];
    }

    FreeNode *freeList_;
    std::vector<FreeNode *> blocks_;
    std::size_t totalAllocated_;
    std::size_t totalFreed_;
};

// ─────────────────────────────────────────────────────────────────
//  线程安全的固定大小对象池
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 线程安全版本（带 mutex）
 *
 * 适用于跨线程共享的对象池。
 */
template <typename T, std::size_t BlockSize = 1024> class ConcurrentFixedSizePool {
  public:
    T *allocate() {
        std::lock_guard<std::mutex> lock(mu_);
        return pool_.allocate();
    }

    void deallocate(T *ptr) noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        pool_.deallocate(ptr);
    }

    template <typename... Args> T *construct(Args &&...args) {
        T *ptr = allocate();
        new (ptr) T(std::forward<Args>(args)...);
        return ptr;
    }

    void destroy(T *ptr) noexcept {
        if (!ptr)
            return;
        ptr->~T();
        deallocate(ptr);
    }

    auto stats() const {
        std::lock_guard<std::mutex> lock(mu_);
        return pool_.stats();
    }

  private:
    mutable std::mutex mu_;
    FixedSizePool<T, BlockSize> pool_;
};

// ─────────────────────────────────────────────────────────────────
//  Slab Allocator (多档位分级)
// ─────────────────────────────────────────────────────────────────

/**
 * @brief Slab 分级分配器
 *
 * 维护多个不同大小档位的内存池：
 *   8B, 16B, 32B, 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB
 *
 * 分配时按需求大小向上取整到最近档位，从对应 slab 分配。
 * 大于 4KB 的请求 fallback 到 ::operator new。
 *
 * 参考：Linux SLUB、jemalloc 的 size class 设计
 */
class SlabAllocator {
  public:
    static constexpr std::size_t kNumClasses = 10;
    static constexpr std::array<std::size_t, kNumClasses> kSizeClasses = {
        8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    static constexpr std::size_t kMaxSlabSize = 4096;

    SlabAllocator() {
        for (std::size_t i = 0; i < kNumClasses; ++i) {
            slabs_[i] = std::make_unique<SlabClass>(kSizeClasses[i]);
        }
    }

    ~SlabAllocator() = default;

    SlabAllocator(const SlabAllocator &) = delete;
    SlabAllocator &operator=(const SlabAllocator &) = delete;

    /**
     * @brief 分配 size 字节内存
     */
    void *allocate(std::size_t size) {
        if (size == 0)
            return nullptr;
        if (size > kMaxSlabSize) {
            // 大对象走系统分配
            ++largeAllocs_;
            return ::operator new(size);
        }

        std::size_t classIdx = sizeClassIndex(size);
        SlabClass &slab = *slabs_[classIdx];

        std::lock_guard<std::mutex> lock(slab.mu);
        if (!slab.freeList) {
            slab.allocateChunk();
        }

        void *ptr = slab.freeList;
        slab.freeList = *reinterpret_cast<void **>(slab.freeList);
        ++slab.allocCount;
        return ptr;
    }

    /**
     * @brief 释放内存
     *
     * @param ptr 通过 allocate 分配的指针
     * @param size 原始请求大小（必须与 allocate 时一致！）
     */
    void deallocate(void *ptr, std::size_t size) noexcept {
        if (!ptr)
            return;
        if (size > kMaxSlabSize) {
            ++largeFrees_;
            ::operator delete(ptr);
            return;
        }

        std::size_t classIdx = sizeClassIndex(size);
        SlabClass &slab = *slabs_[classIdx];

        std::lock_guard<std::mutex> lock(slab.mu);
        *reinterpret_cast<void **>(ptr) = slab.freeList;
        slab.freeList = ptr;
        ++slab.freeCount;
    }

    /**
     * @brief 全局统计
     */
    struct Stats {
        std::array<std::size_t, kNumClasses> classAllocs{};
        std::array<std::size_t, kNumClasses> classFrees{};
        std::array<std::size_t, kNumClasses> classChunks{};
        std::size_t largeAllocs{0};
        std::size_t largeFrees{0};
    };

    Stats stats() const {
        Stats s;
        for (std::size_t i = 0; i < kNumClasses; ++i) {
            std::lock_guard<std::mutex> lock(slabs_[i]->mu);
            s.classAllocs[i] = slabs_[i]->allocCount;
            s.classFrees[i] = slabs_[i]->freeCount;
            s.classChunks[i] = slabs_[i]->chunks.size();
        }
        s.largeAllocs = largeAllocs_;
        s.largeFrees = largeFrees_;
        return s;
    }

    /**
     * @brief 全局单例
     */
    static SlabAllocator &instance() {
        static SlabAllocator inst;
        return inst;
    }

  private:
    struct SlabClass {
        std::size_t blockSize;
        std::size_t chunkObjects{256}; // 每个 chunk 的对象数
        void *freeList{nullptr};
        std::vector<void *> chunks; // 已分配的大块内存
        std::size_t allocCount{0};
        std::size_t freeCount{0};
        mutable std::mutex mu;

        explicit SlabClass(std::size_t bs) : blockSize(bs) {
            // 确保 blockSize >= sizeof(void*)
            if (blockSize < sizeof(void *))
                blockSize = sizeof(void *);
        }

        ~SlabClass() {
            for (auto *c : chunks) {
                ::operator delete(c);
            }
        }

        void allocateChunk() {
            char *chunk = static_cast<char *>(::operator new(blockSize * chunkObjects));
            chunks.push_back(chunk);

            // 构建 free list
            for (std::size_t i = 0; i < chunkObjects - 1; ++i) {
                *reinterpret_cast<void **>(chunk + i * blockSize) = chunk + (i + 1) * blockSize;
            }
            *reinterpret_cast<void **>(chunk + (chunkObjects - 1) * blockSize) = freeList;
            freeList = chunk;
        }
    };

    static std::size_t sizeClassIndex(std::size_t size) noexcept {
        for (std::size_t i = 0; i < kNumClasses; ++i) {
            if (size <= kSizeClasses[i])
                return i;
        }
        return kNumClasses - 1;
    }

    std::array<std::unique_ptr<SlabClass>, kNumClasses> slabs_;
    std::atomic<std::size_t> largeAllocs_{0};
    std::atomic<std::size_t> largeFrees_{0};
};

// ─────────────────────────────────────────────────────────────────
//  STL Allocator 适配器
// ─────────────────────────────────────────────────────────────────

/**
 * @brief STL 兼容的 Slab 分配器
 *
 * 用法：
 * @code
 * std::vector<int, SlabSTLAllocator<int>> vec;
 * @endcode
 */
template <typename T> class SlabSTLAllocator {
  public:
    using value_type = T;

    SlabSTLAllocator() noexcept = default;

    template <typename U> SlabSTLAllocator(const SlabSTLAllocator<U> &) noexcept {}

    T *allocate(std::size_t n) {
        return static_cast<T *>(SlabAllocator::instance().allocate(n * sizeof(T)));
    }

    void deallocate(T *p, std::size_t n) noexcept {
        SlabAllocator::instance().deallocate(p, n * sizeof(T));
    }

    template <typename U> bool operator==(const SlabSTLAllocator<U> &) const noexcept {
        return true;
    }

    template <typename U> bool operator!=(const SlabSTLAllocator<U> &) const noexcept {
        return false;
    }
};

} // namespace mcpp::memory
