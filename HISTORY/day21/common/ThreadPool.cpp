#include "ThreadPool.h"
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

// 构造函数职责：
ThreadPool::ThreadPool(size_t threads) : stop_(false) {
    // 对于每个线程
    for (size_t i = 0; i < threads; ++i)
        //
        workers_.emplace_back([this] {
            // 死循环
            for (;;) {
                // 定义一个临时变量（类型是一个 void 无参数函数）
                std::function<void()> task;
                {
                    // 拿锁
                    std::unique_lock<std::mutex> lock(this->queue_mutex_);
                    // 检查睡觉条件
                    // 要么线程池停止，要么无任务
                    this->condition_.wait(lock,
                                          [this] { return this->stop_ || !this->tasks_.empty(); });
                    // 销毁条件判断
                    if (this->stop_ && this->tasks_.empty())
                        return;
                    // 消费一个任务
                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                }
                // 在临界区外执行业务逻辑
                task();
            }
        });
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    // 所有线程都被唤醒，从 wait 向下执行
    condition_.notify_all();
    for (std::thread &worker : workers_) {
        if (worker.joinable())
            worker.join();
    } // 阻塞直到每个线程报告执行完毕
}