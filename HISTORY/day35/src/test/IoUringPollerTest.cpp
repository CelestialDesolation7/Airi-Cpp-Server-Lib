/**
 * @file IoUringPollerTest.cpp
 * @brief io_uring Poller 单元测试
 *
 * @note 大部分测试只在 Linux + io_uring 可用时运行
 */

#include "Poller/IoUringPoller.h"
#include <gtest/gtest.h>

using namespace mcpp::net;

// ─────────────────────────────────────────────────────────────────
//  跨平台测试
// ─────────────────────────────────────────────────────────────────

TEST(IoUringPollerTest, IsAvailable_DoesNotCrash) {
    // 在 macOS 上应返回 false
    bool available = IoUringPoller::isAvailable();

#ifdef __linux__
    // Linux 上：取决于内核版本和 liburing 是否编译
    SUCCEED() << "io_uring available: " << available;
#else
    EXPECT_FALSE(available);
#endif
}

#if defined(__linux__) && defined(MCPP_HAS_IO_URING)

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────
//  Linux + io_uring 专用测试
// ─────────────────────────────────────────────────────────────────

TEST(IoUringPollerTest, ConstructAndDestroy) {
    if (!IoUringPoller::isAvailable()) {
        GTEST_SKIP() << "io_uring not available on this kernel";
    }

    // 由于 Eventloop 依赖较重，这里只测试构造/析构不崩溃
    // 真实集成测试在 EventLoop 层面进行
    EXPECT_NO_THROW({ IoUringPoller poller(nullptr, 64); });
}

TEST(IoUringPollerTest, StatsInitiallyZero) {
    if (!IoUringPoller::isAvailable()) {
        GTEST_SKIP() << "io_uring not available";
    }

    IoUringPoller poller(nullptr, 64);
    auto stats = poller.stats();
    EXPECT_EQ(stats.submittedOps, 0u);
    EXPECT_EQ(stats.completedOps, 0u);
    EXPECT_EQ(stats.pollAddOps, 0u);
}

#else

TEST(IoUringPollerTest, NotAvailableOnThisPlatform) { EXPECT_FALSE(IoUringPoller::isAvailable()); }

#endif
