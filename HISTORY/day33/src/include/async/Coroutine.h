#pragma once
/**
 * @file Coroutine.h
 * @brief C++20 协程 I/O 支持 (仅在编译器支持时启用)
 *
 * 提供 Task<T> 返回类型、Awaitable 接口，以及异步 I/O 原语。
 * 当编译器不支持 C++20 协程时，此头文件定义为空或提供 fallback。
 *
 * @note Requires -std=c++20 or -fcoroutines (GCC)
 */

#include <version>

// 检测协程支持
#if defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L
#define MCPP_HAS_COROUTINES 1
#else
#define MCPP_HAS_COROUTINES 0
#endif

#if MCPP_HAS_COROUTINES

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace mcpp::coro {

/**
 * @brief 协程任务状态
 */
enum class TaskState {
    kPending, ///< 尚未开始或挂起中
    kReady,   ///< 已完成，结果可用
    kError    ///< 异常终止
};

/**
 * @brief 协程任务返回类型
 *
 * Task<T> 是一个 lazy coroutine，不会立即执行。
 * 调用 co_await task 或 task.get() 时开始执行。
 *
 * @tparam T 返回值类型，支持 void
 *
 * @code
 * Task<int> asyncCompute() {
 *     co_return 42;
 * }
 *
 * Task<void> asyncMain() {
 *     int result = co_await asyncCompute();
 * }
 * @endcode
 */
template <typename T = void> class Task;

// ─────────────────────────────────────────────────────────────────
//  Promise 类型
// ─────────────────────────────────────────────────────────────────

namespace detail {

/**
 * @brief 非 void 返回值的 promise
 */
template <typename T> struct TaskPromise {
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    using ResultType = std::variant<std::monostate, T, std::exception_ptr>;

    ResultType result_;
    std::coroutine_handle<> continuation_{nullptr};

    Task<T> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(Handle h) noexcept {
                if (auto cont = h.promise().continuation_; cont)
                    return cont;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        return FinalAwaiter{};
    }

    void unhandled_exception() noexcept { result_.template emplace<2>(std::current_exception()); }

    template <typename U> void return_value(U &&val) {
        result_.template emplace<1>(std::forward<U>(val));
    }

    T getResult() {
        if (result_.index() == 2)
            std::rethrow_exception(std::get<2>(result_));
        return std::move(std::get<1>(result_));
    }

    bool hasResult() const noexcept { return result_.index() != 0; }
};

/**
 * @brief void 返回值的 promise 特化
 */
template <> struct TaskPromise<void> {
    using Handle = std::coroutine_handle<TaskPromise<void>>;

    std::exception_ptr exception_{nullptr};
    std::coroutine_handle<> continuation_{nullptr};
    bool completed_{false};

    Task<void> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(Handle h) noexcept {
                if (auto cont = h.promise().continuation_; cont)
                    return cont;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        return FinalAwaiter{};
    }

    void unhandled_exception() noexcept { exception_ = std::current_exception(); }

    void return_void() noexcept { completed_ = true; }

    void getResult() {
        if (exception_)
            std::rethrow_exception(exception_);
    }

    bool hasResult() const noexcept { return completed_ || exception_; }
};

} // namespace detail

// ─────────────────────────────────────────────────────────────────
//  Task<T> 实现
// ─────────────────────────────────────────────────────────────────

template <typename T> class Task {
  public:
    using promise_type = detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept : handle_(nullptr) {}

    explicit Task(Handle h) noexcept : handle_(h) {}

    Task(Task &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (handle_)
                handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    ~Task() {
        if (handle_)
            handle_.destroy();
    }

    /**
     * @brief 检查协程是否已完成
     */
    bool done() const noexcept { return handle_ && handle_.done(); }

    /**
     * @brief 使 Task 可被 co_await
     */
    auto operator co_await() && noexcept {
        struct Awaiter {
            Handle handle_;

            bool await_ready() noexcept { return false; }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
                handle_.promise().continuation_ = awaiting;
                return handle_;
            }

            T await_resume() { return handle_.promise().getResult(); }
        };
        return Awaiter{handle_};
    }

    /**
     * @brief 同步获取结果 (阻塞直到完成)
     */
    T syncWait() {
        if (!handle_) {
            if constexpr (std::is_void_v<T>)
                return;
            else
                throw std::runtime_error("Task has no coroutine");
        }
        // 简单实现：busy-wait 执行
        while (!handle_.done()) {
            handle_.resume();
        }
        return handle_.promise().getResult();
    }

    /**
     * @brief 释放 handle 所有权
     */
    Handle release() noexcept {
        auto h = handle_;
        handle_ = nullptr;
        return h;
    }

    Handle handle() const noexcept { return handle_; }

  private:
    Handle handle_;
};

// ─────────────────────────────────────────────────────────────────
//  Promise::get_return_object 实现
// ─────────────────────────────────────────────────────────────────

namespace detail {

template <typename T> Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{Handle::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{Handle::from_promise(*this)};
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────
//  Awaitable 接口
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 基础 Awaitable 概念检查
 */
template <typename T>
concept Awaitable = requires(T t, std::coroutine_handle<> h) {
    { t.await_ready() } -> std::convertible_to<bool>;
    { t.await_suspend(h) };
    { t.await_resume() };
};

/**
 * @brief 立即完成的 Awaitable
 */
template <typename T> struct ReadyAwaitable {
    T value_;

    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    T await_resume() noexcept { return std::move(value_); }
};

template <> struct ReadyAwaitable<void> {
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() noexcept {}
};

/**
 * @brief 用于异步 I/O 的 Awaitable
 *
 * 在 await_suspend 中注册回调，I/O 完成后恢复协程。
 */
template <typename T> class AsyncAwaitable {
  public:
    using Callback = std::function<void(T)>;
    using Initiator = std::function<void(Callback)>;

    explicit AsyncAwaitable(Initiator init) : initiator_(std::move(init)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        initiator_([this](T result) {
            result_.emplace(std::move(result));
            if (handle_)
                handle_.resume();
        });
    }

    T await_resume() { return std::move(*result_); }

  private:
    Initiator initiator_;
    std::coroutine_handle<> handle_;
    std::optional<T> result_;
};

/**
 * @brief void 特化
 */
template <> class AsyncAwaitable<void> {
  public:
    using Callback = std::function<void()>;
    using Initiator = std::function<void(Callback)>;

    explicit AsyncAwaitable(Initiator init) : initiator_(std::move(init)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        initiator_([this]() {
            completed_ = true;
            if (handle_)
                handle_.resume();
        });
    }

    void await_resume() {}

  private:
    Initiator initiator_;
    std::coroutine_handle<> handle_;
    bool completed_{false};
};

// ─────────────────────────────────────────────────────────────────
//  调度器接口
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 协程调度器接口
 *
 * 负责将协程恢复操作投递到正确的执行上下文（如 EventLoop）。
 */
class Scheduler {
  public:
    virtual ~Scheduler() = default;

    /**
     * @brief 投递协程恢复操作
     */
    virtual void schedule(std::coroutine_handle<> h) = 0;

    /**
     * @brief 返回切换到此调度器的 Awaitable
     */
    auto switchTo() {
        struct SwitchAwaiter {
            Scheduler *scheduler_;

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) { scheduler_->schedule(h); }

            void await_resume() noexcept {}
        };
        return SwitchAwaiter{this};
    }
};

// ─────────────────────────────────────────────────────────────────
//  工具函数
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 同步等待一个协程完成
 */
template <typename T> T syncWait(Task<T> task) { return task.syncWait(); }

/**
 * @brief 创建一个已完成的 Task
 */
template <typename T> Task<T> makeReady(T value) { co_return value; }

inline Task<void> makeReady() { co_return; }

/**
 * @brief 延迟执行，用于 yield
 */
inline auto suspend() {
    struct SuspendAwaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept {}
    };
    return SuspendAwaiter{};
}

} // namespace mcpp::coro

#else // !MCPP_HAS_COROUTINES

// 协程不可用时提供空命名空间
namespace mcpp::coro {
// Coroutines not available. Compile with C++20 or later.
}

#endif // MCPP_HAS_COROUTINES
