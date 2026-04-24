#pragma once
/**
 * @file WorkStealingPool.h
 * @brief Work-Stealing 线程池
 *
 * 经典的 work-stealing 调度器实现：
 * - 每个 worker 持有自己的双端队列（deque）
 * - 本地任务从 deque 头部 push/pop（LIFO，缓存友好）
 * - 空闲 worker 从其他 worker 的 deque 尾部偷任务（FIFO）
 * - 全局任务队列处理外部提交
 *
 * 参考：Cilk-5 调度器、Java ForkJoinPool
 */

#include "LockFreeQueue.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace mcpp::async {

/**
 * @brief Work-Stealing 双端队列
 *
 * 简化版（带锁）实现，便于稳定性。
 * 真正无锁的 Chase-Lev deque 留作后续优化。
 *
 * @note Owner 线程从 bottom 端 push/pop，stealer 线程从 top 端 steal
 */
class WorkStealingDeque {
  public:
    using Task = std::function<void()>;

    /**
     * @brief Owner 入队（push 到 bottom）
     */
    void push(Task task) {
        std::lock_guard<std::mutex> lock(mu_);
        deque_.push_back(std::move(task));
    }

    /**
     * @brief Owner 出队（pop 从 bottom，LIFO）
     */
    bool pop(Task &out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (deque_.empty())
            return false;
        out = std::move(deque_.back());
        deque_.pop_back();
        return true;
    }

    /**
     * @brief Stealer 出队（steal 从 top，FIFO）
     */
    bool steal(Task &out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (deque_.empty())
            return false;
        out = std::move(deque_.front());
        deque_.pop_front();
        return true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return deque_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return deque_.empty();
    }

  private:
    mutable std::mutex mu_;
    std::deque<Task> deque_;
};

/**
 * @brief Work-Stealing 线程池
 *
 * 使用示例：
 * @code
 * WorkStealingPool pool(8);
 * auto fut = pool.submit([]() { return 42; });
 * int result = fut.get();
 * @endcode
 */
class WorkStealingPool {
  public:
    using Task = std::function<void()>;

    /**
     * @brief 构造函数
     * @param numThreads worker 数量，默认硬件并发数
     */
    explicit WorkStealingPool(std::size_t numThreads = 0) {
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0)
                numThreads = 4;
        }
        numThreads_ = numThreads;

        // 创建每个 worker 的 deque
        for (std::size_t i = 0; i < numThreads; ++i) {
            workerDeques_.emplace_back(std::make_unique<WorkStealingDeque>());
        }

        // 启动 worker 线程
        running_.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
    }

    ~WorkStealingPool() { shutdown(); }

    WorkStealingPool(const WorkStealingPool &) = delete;
    WorkStealingPool &operator=(const WorkStealingPool &) = delete;

    /**
     * @brief 提交任务（外部线程调用）
     *
     * 任务进入全局队列，由空闲 worker 拉取。
     */
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))> {
        using Ret = decltype(f(args...));

        auto task = std::make_shared<std::packaged_task<Ret()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(globalMu_);
            globalQueue_.push_back([task]() { (*task)(); });
        }
        cv_.notify_one();

        return future;
    }

    /**
     * @brief 在 worker 内部派发本地任务（fork-join 风格）
     *
     * 当前线程必须是 worker 线程。
     */
    void dispatch(Task task) {
        // 简化：始终走全局队列
        std::lock_guard<std::mutex> lock(globalMu_);
        globalQueue_.push_back(std::move(task));
        cv_.notify_one();
    }

    /**
     * @brief 关闭线程池，等待所有 worker 退出
     */
    void shutdown() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return; // 已关闭
        }

        cv_.notify_all();
        for (auto &t : workers_) {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
    }

    std::size_t numThreads() const noexcept { return numThreads_; }

    /**
     * @brief 统计信息
     */
    struct Stats {
        std::atomic<uint64_t> tasksExecuted{0};
        std::atomic<uint64_t> tasksStolen{0};
        std::atomic<uint64_t> tasksFromGlobal{0};
    };

    const Stats &stats() const noexcept { return stats_; }

  private:
    void workerLoop(std::size_t id) {
        std::mt19937 rng(static_cast<unsigned>(id) * 31 + 7);

        while (running_.load(std::memory_order_acquire)) {
            Task task;

            // 1. 优先从本地 deque 取任务（LIFO，缓存友好）
            if (workerDeques_[id]->pop(task)) {
                task();
                stats_.tasksExecuted.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // 2. 尝试从全局队列拉取
            if (popFromGlobal(task)) {
                stats_.tasksFromGlobal.fetch_add(1, std::memory_order_relaxed);
                task();
                stats_.tasksExecuted.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // 3. 尝试从其他 worker 偷任务
            if (stealFromOthers(id, rng, task)) {
                stats_.tasksStolen.fetch_add(1, std::memory_order_relaxed);
                task();
                stats_.tasksExecuted.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // 4. 没有任务，等待
            std::unique_lock<std::mutex> lock(globalMu_);
            cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                return !running_.load(std::memory_order_acquire) || !globalQueue_.empty();
            });
        }
    }

    bool popFromGlobal(Task &task) {
        std::lock_guard<std::mutex> lock(globalMu_);
        if (globalQueue_.empty())
            return false;
        task = std::move(globalQueue_.front());
        globalQueue_.pop_front();
        return true;
    }

    bool stealFromOthers(std::size_t myId, std::mt19937 &rng, Task &task) {
        if (numThreads_ <= 1)
            return false;

        // 随机选择 victim
        std::uniform_int_distribution<std::size_t> dist(0, numThreads_ - 1);
        for (std::size_t attempts = 0; attempts < numThreads_; ++attempts) {
            std::size_t victim = dist(rng);
            if (victim == myId)
                continue;

            if (workerDeques_[victim]->steal(task)) {
                return true;
            }
        }
        return false;
    }

    std::size_t numThreads_;
    std::vector<std::thread> workers_;
    std::vector<std::unique_ptr<WorkStealingDeque>> workerDeques_;

    std::deque<Task> globalQueue_;
    std::mutex globalMu_;
    std::condition_variable cv_;

    std::atomic<bool> running_{false};
    Stats stats_;
};

} // namespace mcpp::async
