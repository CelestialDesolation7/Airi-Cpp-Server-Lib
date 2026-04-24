/**
 * @file MemoryPoolTest.cpp
 * @brief 内存池单元测试
 */

#include "memory/MemoryPool.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace mcpp::memory;

// ─────────────────────────────────────────────────────────────────
//  FixedSizePool 测试
// ─────────────────────────────────────────────────────────────────

struct TestObj {
    int a;
    double b;
    char c[40];

    TestObj(int x = 0) : a(x), b(x * 1.5) {
        for (int i = 0; i < 40; ++i)
            c[i] = static_cast<char>(i);
    }
};

TEST(FixedSizePoolTest, BasicAllocateDeallocate) {
    FixedSizePool<TestObj, 16> pool;

    auto stats0 = pool.stats();
    EXPECT_EQ(stats0.inUse, 0u);

    TestObj *p1 = pool.allocate();
    TestObj *p2 = pool.allocate();
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    auto stats1 = pool.stats();
    EXPECT_EQ(stats1.inUse, 2u);

    pool.deallocate(p1);
    pool.deallocate(p2);

    auto stats2 = pool.stats();
    EXPECT_EQ(stats2.inUse, 0u);
}

TEST(FixedSizePoolTest, ConstructDestroy) {
    FixedSizePool<TestObj, 16> pool;

    TestObj *obj = pool.construct(42);
    EXPECT_EQ(obj->a, 42);
    EXPECT_DOUBLE_EQ(obj->b, 63.0);

    pool.destroy(obj);
}

TEST(FixedSizePoolTest, BlockExpansion) {
    FixedSizePool<TestObj, 4> pool; // 每个 block 4 个对象

    std::vector<TestObj *> ptrs;
    for (int i = 0; i < 10; ++i) {
        ptrs.push_back(pool.allocate());
    }

    auto stats = pool.stats();
    EXPECT_GE(stats.blocksAllocated, 3u); // 至少 3 个 block (4*3=12 >= 10)
    EXPECT_EQ(stats.inUse, 10u);

    for (auto *p : ptrs)
        pool.deallocate(p);
}

TEST(FixedSizePoolTest, ReuseAfterFree) {
    FixedSizePool<TestObj, 16> pool;

    TestObj *p1 = pool.allocate();
    pool.deallocate(p1);
    TestObj *p2 = pool.allocate(); // 应复用 p1 的内存

    EXPECT_EQ(p1, p2);
    pool.deallocate(p2);
}

TEST(FixedSizePoolTest, ManyAllocs) {
    FixedSizePool<int, 64> pool;
    constexpr int kN = 1000;

    std::vector<int *> ptrs;
    ptrs.reserve(kN);

    for (int i = 0; i < kN; ++i) {
        int *p = pool.construct(i);
        ptrs.push_back(p);
    }

    for (int i = 0; i < kN; ++i) {
        EXPECT_EQ(*ptrs[i], i);
    }

    for (auto *p : ptrs)
        pool.destroy(p);

    EXPECT_EQ(pool.stats().inUse, 0u);
}

// ─────────────────────────────────────────────────────────────────
//  ConcurrentFixedSizePool 测试
// ─────────────────────────────────────────────────────────────────

TEST(ConcurrentFixedSizePoolTest, MultiThreadedAllocFree) {
    ConcurrentFixedSizePool<int, 64> pool;

    constexpr int kThreads = 4;
    constexpr int kPerThread = 10000;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kPerThread; ++i) {
                int *p = pool.construct(i);
                if (*p != i)
                    errors.fetch_add(1);
                pool.destroy(p);
            }
        });
    }

    for (auto &th : threads)
        th.join();
    EXPECT_EQ(errors.load(), 0);
}

// ─────────────────────────────────────────────────────────────────
//  SlabAllocator 测试
// ─────────────────────────────────────────────────────────────────

TEST(SlabAllocatorTest, SmallAllocations) {
    SlabAllocator allocator;

    void *p1 = allocator.allocate(8);
    void *p2 = allocator.allocate(16);
    void *p3 = allocator.allocate(32);

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p3, nullptr);

    allocator.deallocate(p1, 8);
    allocator.deallocate(p2, 16);
    allocator.deallocate(p3, 32);
}

TEST(SlabAllocatorTest, RoundsUpToSizeClass) {
    SlabAllocator allocator;

    // 请求 10 字节，应分配 16 字节档位
    void *p = allocator.allocate(10);
    EXPECT_NE(p, nullptr);
    allocator.deallocate(p, 10);

    auto stats = allocator.stats();
    EXPECT_GE(stats.classAllocs[1], 1u); // class 1 = 16B
}

TEST(SlabAllocatorTest, LargeAllocFallback) {
    SlabAllocator allocator;

    void *p = allocator.allocate(8192); // > 4KB，走系统
    EXPECT_NE(p, nullptr);
    allocator.deallocate(p, 8192);

    auto stats = allocator.stats();
    EXPECT_EQ(stats.largeAllocs, 1u);
    EXPECT_EQ(stats.largeFrees, 1u);
}

TEST(SlabAllocatorTest, ZeroSize) {
    SlabAllocator allocator;
    void *p = allocator.allocate(0);
    EXPECT_EQ(p, nullptr);
    allocator.deallocate(nullptr, 0); // 不应崩溃
}

TEST(SlabAllocatorTest, ReuseFreeList) {
    SlabAllocator allocator;

    void *p1 = allocator.allocate(64);
    allocator.deallocate(p1, 64);
    void *p2 = allocator.allocate(64);

    EXPECT_EQ(p1, p2); // 应复用
    allocator.deallocate(p2, 64);
}

TEST(SlabAllocatorTest, MultiThreaded) {
    SlabAllocator allocator;

    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            std::vector<std::pair<void *, std::size_t>> ptrs;
            ptrs.reserve(kPerThread);

            for (int i = 0; i < kPerThread; ++i) {
                std::size_t size = 8 << (i % 6); // 8, 16, 32, 64, 128, 256
                void *p = allocator.allocate(size);
                if (!p)
                    errors.fetch_add(1);
                ptrs.emplace_back(p, size);
            }

            for (auto &[p, sz] : ptrs) {
                allocator.deallocate(p, sz);
            }
        });
    }

    for (auto &th : threads)
        th.join();
    EXPECT_EQ(errors.load(), 0);
}

// ─────────────────────────────────────────────────────────────────
//  STL Allocator 适配器测试
// ─────────────────────────────────────────────────────────────────

TEST(SlabSTLAllocatorTest, VectorWorks) {
    std::vector<int, SlabSTLAllocator<int>> vec;

    for (int i = 0; i < 100; ++i) {
        vec.push_back(i);
    }

    EXPECT_EQ(vec.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(vec[i], i);
    }
}

// ─────────────────────────────────────────────────────────────────
//  性能基准
// ─────────────────────────────────────────────────────────────────

TEST(MemoryPoolBenchmark, FixedPoolVsMalloc) {
    constexpr int kN = 100000;

    // FixedSizePool
    FixedSizePool<TestObj, 1024> pool;
    auto start1 = std::chrono::steady_clock::now();
    std::vector<TestObj *> ptrs1;
    ptrs1.reserve(kN);
    for (int i = 0; i < kN; ++i)
        ptrs1.push_back(pool.construct(i));
    for (auto *p : ptrs1)
        pool.destroy(p);
    auto poolTime = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start1)
                        .count();

    // 普通 new/delete
    auto start2 = std::chrono::steady_clock::now();
    std::vector<TestObj *> ptrs2;
    ptrs2.reserve(kN);
    for (int i = 0; i < kN; ++i)
        ptrs2.push_back(new TestObj(i));
    for (auto *p : ptrs2)
        delete p;
    auto newTime = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - start2)
                       .count();

    std::cout << "[BENCHMARK] FixedSizePool: " << poolTime << "us, "
              << "new/delete: " << newTime << "us, "
              << "speedup: " << (double)newTime / poolTime << "x\n";

    // pool 应至少与 malloc 相当（通常更快）
    SUCCEED();
}
