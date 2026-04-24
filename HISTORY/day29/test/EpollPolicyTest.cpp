#ifdef __linux__

#include "Poller/EpollPoller.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/epoll.h>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "[EpollPolicyTest] 失败: " << msg << "\n";
    std::exit(1);
}

void check(bool cond, const std::string &msg) {
    if (!cond)
        fail(msg);
}

void testRetryPolicy() {
    std::cout << "[EpollPolicyTest] 用例1：ADD/MOD 自愈重试策略\n";

    check(EpollPoller::shouldRetryWithMod(EPOLL_CTL_ADD, EEXIST), "ADD + EEXIST 应重试为 MOD");
    check(!EpollPoller::shouldRetryWithMod(EPOLL_CTL_MOD, EEXIST), "MOD + EEXIST 不应重试为 MOD");

    check(EpollPoller::shouldRetryWithAdd(EPOLL_CTL_MOD, ENOENT), "MOD + ENOENT 应重试为 ADD");
    check(!EpollPoller::shouldRetryWithAdd(EPOLL_CTL_ADD, ENOENT), "ADD + ENOENT 不应重试为 ADD");
}

void testIgnorePolicy() {
    std::cout << "[EpollPolicyTest] 用例2：DEL 可恢复错误忽略策略\n";

    check(EpollPoller::shouldIgnoreCtlError(EPOLL_CTL_DEL, ENOENT), "DEL + ENOENT 应忽略");
    check(EpollPoller::shouldIgnoreCtlError(EPOLL_CTL_DEL, EBADF), "DEL + EBADF 应忽略");

    check(!EpollPoller::shouldIgnoreCtlError(EPOLL_CTL_ADD, EEXIST), "ADD + EEXIST 不属于忽略策略");
    check(!EpollPoller::shouldIgnoreCtlError(EPOLL_CTL_MOD, ENOENT), "MOD + ENOENT 不属于忽略策略");
}

} // namespace

int main() {
    std::cout << "[EpollPolicyTest] 开始执行\n";

    testRetryPolicy();
    testIgnorePolicy();

    std::cout << "[EpollPolicyTest] 全部通过\n";
    return 0;
}

#else

int main() { return 0; }

#endif
