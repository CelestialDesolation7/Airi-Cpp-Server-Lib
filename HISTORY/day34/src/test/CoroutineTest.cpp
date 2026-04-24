/**
 * @file CoroutineTest.cpp
 * @brief C++20 协程模块单元测试
 *
 * 测试 Task<T>、Awaitable 和协程基础设施。
 * 仅在支持 C++20 协程的编译器上运行。
 */

#include "async/Coroutine.h"
#include <gtest/gtest.h>

#if MCPP_HAS_COROUTINES

#include <stdexcept>
#include <string>
#include <vector>

using namespace mcpp::coro;

// ─────────────────────────────────────────────────────────────────
//  基础 Task<T> 测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, TaskInt_SimpleReturn) {
    auto coro = []() -> Task<int> { co_return 42; };

    Task<int> task = coro();
    int result = task.syncWait();
    EXPECT_EQ(result, 42);
}

TEST(CoroutineTest, TaskVoid_SimpleReturn) {
    bool executed = false;

    auto coro = [&]() -> Task<void> {
        executed = true;
        co_return;
    };

    Task<void> task = coro();
    task.syncWait();
    EXPECT_TRUE(executed);
}

TEST(CoroutineTest, TaskString_Return) {
    auto coro = []() -> Task<std::string> { co_return "hello coroutine"; };

    auto result = coro().syncWait();
    EXPECT_EQ(result, "hello coroutine");
}

// ─────────────────────────────────────────────────────────────────
//  Task 组合测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, TaskChain_Await) {
    auto inner = []() -> Task<int> { co_return 10; };

    auto outer = [&]() -> Task<int> {
        int val = co_await inner();
        co_return val * 2;
    };

    int result = outer().syncWait();
    EXPECT_EQ(result, 20);
}

TEST(CoroutineTest, TaskMultipleAwait) {
    auto getNum = [](int n) -> Task<int> { co_return n; };

    auto sum = [&]() -> Task<int> {
        int a = co_await getNum(1);
        int b = co_await getNum(2);
        int c = co_await getNum(3);
        co_return a + b + c;
    };

    EXPECT_EQ(sum().syncWait(), 6);
}

TEST(CoroutineTest, TaskNestedThreeLevels) {
    auto level3 = []() -> Task<int> { co_return 5; };

    auto level2 = [&]() -> Task<int> {
        int val = co_await level3();
        co_return val + 10;
    };

    auto level1 = [&]() -> Task<int> {
        int val = co_await level2();
        co_return val + 100;
    };

    EXPECT_EQ(level1().syncWait(), 115);
}

// ─────────────────────────────────────────────────────────────────
//  异常处理测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, TaskException_Propagates) {
    auto coro = []() -> Task<int> {
        throw std::runtime_error("test exception");
        co_return 0; // never reached
    };

    Task<int> task = coro();
    EXPECT_THROW(task.syncWait(), std::runtime_error);
}

TEST(CoroutineTest, TaskVoidException_Propagates) {
    auto coro = []() -> Task<void> {
        throw std::logic_error("void task error");
        co_return;
    };

    EXPECT_THROW(coro().syncWait(), std::logic_error);
}

TEST(CoroutineTest, TaskException_InNestedAwait) {
    auto inner = []() -> Task<int> {
        throw std::runtime_error("inner error");
        co_return 0;
    };

    auto outer = [&]() -> Task<int> {
        int val = co_await inner();
        co_return val;
    };

    EXPECT_THROW(outer().syncWait(), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────
//  ReadyAwaitable 测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, ReadyAwaitable_Int) {
    auto coro = []() -> Task<int> {
        int val = co_await ReadyAwaitable<int>{42};
        co_return val;
    };

    EXPECT_EQ(coro().syncWait(), 42);
}

TEST(CoroutineTest, ReadyAwaitable_Void) {
    bool reached = false;

    auto coro = [&]() -> Task<void> {
        co_await ReadyAwaitable<void>{};
        reached = true;
        co_return;
    };

    coro().syncWait();
    EXPECT_TRUE(reached);
}

// ─────────────────────────────────────────────────────────────────
//  AsyncAwaitable 测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, AsyncAwaitable_Callback) {
    auto coro = []() -> Task<int> {
        int val = co_await AsyncAwaitable<int>([](auto callback) {
            // 模拟异步操作：直接调用回调
            callback(99);
        });
        co_return val;
    };

    EXPECT_EQ(coro().syncWait(), 99);
}

TEST(CoroutineTest, AsyncAwaitable_Void) {
    bool callbackInvoked = false;

    auto coro = [&]() -> Task<void> {
        co_await AsyncAwaitable<void>([&](auto callback) {
            callbackInvoked = true;
            callback();
        });
        co_return;
    };

    coro().syncWait();
    EXPECT_TRUE(callbackInvoked);
}

// ─────────────────────────────────────────────────────────────────
//  Task 移动语义测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, TaskMove_Ownership) {
    auto coro = []() -> Task<int> { co_return 123; };

    Task<int> task1 = coro();
    Task<int> task2 = std::move(task1);

    // task1 应该是空的
    EXPECT_EQ(task1.handle(), nullptr);

    // task2 应该有效
    EXPECT_EQ(task2.syncWait(), 123);
}

// ─────────────────────────────────────────────────────────────────
//  makeReady 测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, MakeReady_Value) {
    auto task = makeReady(42);
    EXPECT_EQ(task.syncWait(), 42);
}

TEST(CoroutineTest, MakeReady_Void) {
    auto task = makeReady();
    task.syncWait(); // 不应抛异常
}

// ─────────────────────────────────────────────────────────────────
//  suspend 测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, Suspend_YieldsControl) {
    int step = 0;

    auto coro = [&]() -> Task<void> {
        step = 1;
        co_await suspend();
        step = 2;
        co_return;
    };

    Task<void> task = coro();
    EXPECT_EQ(step, 0); // lazy, 尚未执行

    // resume 第一次
    task.handle().resume();
    EXPECT_EQ(step, 1);

    // resume 第二次
    task.handle().resume();
    EXPECT_EQ(step, 2);
}

// ─────────────────────────────────────────────────────────────────
//  复杂场景测试
// ─────────────────────────────────────────────────────────────────

TEST(CoroutineTest, TaskVector_AccumulateResults) {
    auto gen = [](int n) -> Task<int> { co_return n *n; };

    auto collect = [&]() -> Task<std::vector<int>> {
        std::vector<int> results;
        for (int i = 1; i <= 5; ++i) {
            results.push_back(co_await gen(i));
        }
        co_return results;
    };

    auto result = collect().syncWait();
    std::vector<int> expected = {1, 4, 9, 16, 25};
    EXPECT_EQ(result, expected);
}

TEST(CoroutineTest, TaskConditional_EarlyReturn) {
    auto coro = [](bool flag) -> Task<int> {
        if (flag) {
            co_return 1;
        }
        co_return 0;
    };

    EXPECT_EQ(coro(true).syncWait(), 1);
    EXPECT_EQ(coro(false).syncWait(), 0);
}

#else // !MCPP_HAS_COROUTINES

// 协程不可用时跳过测试
TEST(CoroutineTest, NotAvailable) { GTEST_SKIP() << "C++20 coroutines not available"; }

#endif // MCPP_HAS_COROUTINES
