#include "ThreadPool.h"
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

// 构造函数职责：
ThreadPool::ThreadPool(size_t threads) : stop(false) {
    // 对于每个线程
    for (size_t i = 0; i < threads; ++i)
        //
        workers.emplace_back([this] {
            // 死循环
            for (;;) {
                // 定义一个临时变量（类型是一个 void 无参数函数）
                std::function<void()> task;
                {
                    // 拿锁
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    // 检查睡觉条件
                    // 要么线程池停止，要么无任务
                    this->condition.wait(lock,
                                         [this] { return this->stop || !this->tasks.empty(); });
                    // 销毁条件判断
                    if (this->stop && this->tasks.empty())
                        return;
                    // 消费一个任务
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                // 在临界区外执行业务逻辑
                task();
            }
        });
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    // 所有线程都被唤醒，从 wait 向下执行
    condition.notify_all();
    for (std::thread &worker : workers) {
        if (worker.joinable())
            worker.join();
    } // 阻塞直到每个线程报告执行完毕
}