#pragma once
#include "Macros.h"
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
    DISALLOW_COPY_AND_MOVE(ThreadPool)
  public:
    ThreadPool(size_t thread = 4);
    ~ThreadPool();

    template <class F, class... Args>
    auto add(F &&f, Args &&...args) -> std::future<typename std::result_of<F(Args...)>::type>;
    // 定义一个函数叫 add。它能接收任何可调用对象 f，以及任意多个参数 args。
    // 它的返回类型是 std::future，里面包裹的类型，就是 f 运行后产生的那个结果类型。

  private:
    // 线程数组，默认初始化时被启动
    std::vector<std::thread> workers_;
    // 待处理业务逻辑函数
    std::queue<std::function<void()>> tasks_;

    std::mutex queue_mutex_;
    std::condition_variable condition_;
    // 当且仅当该字段被外部置为 true 时，线程池析构
    bool stop_;
};

// 模板实现
template <class F, class... Args>
auto ThreadPool::add(F &&f, Args &&...args)
    -> std::future<typename std::result_of<F(Args...)>::type> {

    // 编译期计算：如果用参数 Args 调用 F，返回值的类型
    using return_type = typename std::result_of<F(Args...)>::type;

    // 1. std::forward: 完美转发。如果 args 是右值（临时对象），保持其右值属性，
    //    以便 bind 能移动它们而不是复制它们（性能优化）。
    // 2. std::bind: 将函数 f 和参数 args 绑定。封装为无参数函数。
    //    结果是一个“函数对象”，调用它不需要参数，它内部保存了 f 和 args 的副本。
    // 3. std::packaged_task: 异步任务包装器。
    //    它的模板参数是 return_type()，表示它包裹的是一个“无参、返回 return_type”的函数。
    //    它负责在执行 bind 对象后，捕获返回值或异常，并填入共享状态（Shared State）。
    // 4. std::make_shared: 在堆上分配内存。
    //    原因：packaged_task 是不可复制的（non-copyable）。
    //    但我们的 tasks 队列（std::function）通常要求对象可复制。
    //    通过 shared_ptr，我们可以复制指针，从而间接让任务“可复制”地进入队列。
    // 5. 结局：task 是一个共享指针类型的值，指向我们费力构造出来的异步任务。
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    // 从 packaged_task 的共享状态中获取 future。
    // 将来任务执行完，结果会写入共享状态，持有 res 的线程就能通过 future 提供的 get() 读到。
    std::future<return_type> res = task->get_future();

    {
        // RAII 锁：构造时调用 mutex.lock()，作用域结束析构时自动 mutex.unlock()
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // 如果线程池已经停止（stop 为 true），则禁止添加新任务，防止任务执行到一半线程池销毁
        if (stop_)
            throw std::runtime_error("[server] enqueue on stopped ThreadPool");

        // 我们构建一个 Lambda 表达式：
        //   1. [task]：按值捕获 shared_ptr。引用计数 +1。
        //      这确保了即使 add 函数结束，堆上的 packaged_task 也不会被释放，
        //      因为它被 Lambda 持有着，而 Lambda 被队列持有着。
        //   2. () { (*task)(); }：这是 Lambda 的函数体。
        //      当工作线程执行这个 Lambda 时，它解引用指针，调用 packaged_task 的 operator()。
        //      这将触发 bind 对象的执行，并将结果写入 future。
        // 这个 Lambda 的类型是 void()，符合队列的静态类型要求。
        tasks_.emplace([task]() { (*task)(); });
    }

    // 调用系统调用（如 Linux 的 futex），唤醒一个正在 condition.wait() 的工作线程。
    condition_.notify_one();

    // 立即返回 future 对象。
    // 此时任务可能还没开始执行。
    return res;
}