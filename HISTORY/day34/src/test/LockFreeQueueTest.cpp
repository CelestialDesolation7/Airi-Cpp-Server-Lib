/**
 * @file LockFreeQueueTest.cpp
 * @brief 无锁队列与 work-stealing 线程池单元测试
 */

#include "async/LockFreeQueue.h"
#include "async/WorkStealingPool.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <thread>
#include <vector>

using namespace mcpp::async;

// ─────────────────────────────────────────────────────────────────
//  SPSCQueue 测试
// ─────────────────────────────────────────────────────────────────

TEST(SPSCQueueTest, BasicPushPop) {
    SPSCQueue<int> q(16);

    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_TRUE(q.tryPush(3));
    EXPECT_EQ(q.size(), 3u);

    int val;
    EXPECT_TRUE(q.tryPop(val));
    EXPECT_EQ(val, 1);
    EXPECT_TRUE(q.tryPop(val));
    EXPECT_EQ(val, 2);
    EXPECT_TRUE(q.tryPop(val));
    EXPECT_EQ(val, 3);
    EXPECT_FALSE(q.tryPop(val));
}

TEST(SPSCQueueTest, CapacityRoundedToPowerOf2) {
    SPSCQueue<int> q(100); // 应被向上取整到 128
    EXPECT_EQ(q.capacity(), 128u);
}

TEST(SPSCQueueTest, FullQueueRefusesPush) {
    SPSCQueue<int> q(4); // 容量 4
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_TRUE(q.tryPush(3));
    EXPECT_TRUE(q.tryPush(4));
    EXPECT_FALSE(q.tryPush(5)); // 满
}

TEST(SPSCQueueTest, ConcurrentProducerConsumer) {
    constexpr int kN = 100000;
    SPSCQueue<int> q(1024);
    std::atomic<bool> done{false};

    std::thread producer([&]() {
        for (int i = 0; i < kN; ++i) {
            while (!q.tryPush(i)) {
                std::this_thread::yield();
            }
        }
        done.store(true);
    });

    std::vector<int> received;
    received.reserve(kN);
    std::thread consumer([&]() {
        int val;
        while (received.size() < (size_t)kN) {
            if (q.tryPop(val)) {
                received.push_back(val);
            } else if (done.load()) {
                // drain remaining
                while (q.tryPop(val))
                    received.push_back(val);
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(received.size(), (size_t)kN);
    for (int i = 0; i < kN; ++i) {
        EXPECT_EQ(received[i], i);
    }
}

TEST(SPSCQueueTest, MoveOnlyType) {
    SPSCQueue<std::unique_ptr<int>> q(8);

    EXPECT_TRUE(q.tryPush(std::make_unique<int>(42)));

    std::unique_ptr<int> val;
    EXPECT_TRUE(q.tryPop(val));
    ASSERT_TRUE(val);
    EXPECT_EQ(*val, 42);
}

// ─────────────────────────────────────────────────────────────────
//  MPMCQueue 测试
// ─────────────────────────────────────────────────────────────────

TEST(MPMCQueueTest, BasicPushPop) {
    MPMCQueue<int> q(16);

    EXPECT_TRUE(q.tryPush(10));
    EXPECT_TRUE(q.tryPush(20));

    int val;
    EXPECT_TRUE(q.tryPop(val));
    EXPECT_EQ(val, 10);
    EXPECT_TRUE(q.tryPop(val));
    EXPECT_EQ(val, 20);
    EXPECT_FALSE(q.tryPop(val));
}

TEST(MPMCQueueTest, MultipleProducersConsumers) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 10000;
    constexpr int kTotal = kProducers * kPerProducer;

    MPMCQueue<int> q(2048);
    std::atomic<int> totalProduced{0};
    std::atomic<int> totalConsumed{0};
    std::vector<std::vector<int>> received(kConsumers);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < kPerProducer; ++i) {
                int val = p * kPerProducer + i;
                while (!q.tryPush(val)) {
                    std::this_thread::yield();
                }
                totalProduced.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&, c]() {
            int val;
            while (totalConsumed.load() < kTotal) {
                if (q.tryPop(val)) {
                    received[c].push_back(val);
                    totalConsumed.fetch_add(1);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto &t : producers)
        t.join();
    for (auto &t : consumers)
        t.join();

    EXPECT_EQ(totalProduced.load(), kTotal);
    EXPECT_EQ(totalConsumed.load(), kTotal);

    // 验证所有元素都被收到（无重复，无丢失）
    std::set<int> allReceived;
    for (auto &v : received) {
        for (int x : v)
            allReceived.insert(x);
    }
    EXPECT_EQ(allReceived.size(), (size_t)kTotal);
}

TEST(MPMCQueueTest, FullQueueRefuses) {
    MPMCQueue<int> q(4);
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_TRUE(q.tryPush(3));
    EXPECT_TRUE(q.tryPush(4));
    EXPECT_FALSE(q.tryPush(5));
}

// ─────────────────────────────────────────────────────────────────
//  WorkStealingPool 测试
// ─────────────────────────────────────────────────────────────────

TEST(WorkStealingPoolTest, ConstructWithDefaultThreads) {
    WorkStealingPool pool;
    EXPECT_GT(pool.numThreads(), 0u);
}

TEST(WorkStealingPoolTest, SubmitAndGetResult) {
    WorkStealingPool pool(4);

    auto fut = pool.submit([]() { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

TEST(WorkStealingPoolTest, SubmitWithArgs) {
    WorkStealingPool pool(4);

    auto fut = pool.submit([](int a, int b) { return a + b; }, 10, 20);
    EXPECT_EQ(fut.get(), 30);
}

TEST(WorkStealingPoolTest, SubmitVoidTask) {
    WorkStealingPool pool(4);
    std::atomic<int> counter{0};

    auto fut = pool.submit([&]() { counter.fetch_add(1); });
    fut.get();
    EXPECT_EQ(counter.load(), 1);
}

TEST(WorkStealingPoolTest, ManyTasks) {
    constexpr int kN = 1000;
    WorkStealingPool pool(4);
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        futures.push_back(pool.submit([&]() { counter.fetch_add(1); }));
    }

    for (auto &f : futures)
        f.get();
    EXPECT_EQ(counter.load(), kN);
}

TEST(WorkStealingPoolTest, ParallelSum) {
    constexpr int kN = 1000;
    WorkStealingPool pool(4);

    std::vector<std::future<int>> futures;
    for (int i = 1; i <= kN; ++i) {
        futures.push_back(pool.submit([i]() { return i; }));
    }

    int sum = 0;
    for (auto &f : futures)
        sum += f.get();
    EXPECT_EQ(sum, kN * (kN + 1) / 2);
}

TEST(WorkStealingPoolTest, ShutdownIsIdempotent) {
    WorkStealingPool pool(2);
    pool.shutdown();
    EXPECT_NO_THROW(pool.shutdown()); // 第二次应安全
}

TEST(WorkStealingPoolTest, ExceptionPropagation) {
    WorkStealingPool pool(2);

    auto fut = pool.submit([]() -> int { throw std::runtime_error("task error"); });

    EXPECT_THROW(fut.get(), std::runtime_error);
}

TEST(WorkStealingPoolTest, StatsTracksExecution) {
    WorkStealingPool pool(2);

    constexpr int kN = 100;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kN; ++i) {
        futures.push_back(pool.submit([]() {}));
    }
    for (auto &f : futures)
        f.get();

    // 注意：stats_.tasksExecuted 是在 task() 调用之后才递增的，
    // 而 future.get() 返回仅表示 task 本体执行完毕，不包括后续的 fetch_add。
    // 因此必须轮询等待计数器赶上，不能直接 EXPECT_GE。
    constexpr auto kTimeout = std::chrono::seconds(2);
    auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (pool.stats().tasksExecuted.load() < (uint64_t)kN &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(pool.stats().tasksExecuted.load(), (uint64_t)kN);
}
