#include "TcpServer.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "[TcpServerPolicyTest] 失败: " << msg << "\n";
    std::exit(1);
}

void check(bool cond, const std::string &msg) {
    if (!cond)
        fail(msg);
}

void testRejectBoundary() {
    std::cout << "[TcpServerPolicyTest] 用例1：上限边界判定\n";

    check(!TcpServer::shouldRejectNewConnection(0, 10), "0/10 不应拒绝");
    check(!TcpServer::shouldRejectNewConnection(9, 10), "9/10 不应拒绝");
    check(TcpServer::shouldRejectNewConnection(10, 10), "10/10 应拒绝");
    check(TcpServer::shouldRejectNewConnection(11, 10), "11/10 应拒绝");
}

void testSmallLimit() {
    std::cout << "[TcpServerPolicyTest] 用例2：小上限场景\n";

    check(!TcpServer::shouldRejectNewConnection(0, 1), "0/1 不应拒绝");
    check(TcpServer::shouldRejectNewConnection(1, 1), "1/1 应拒绝");
}

void testLargeValue() {
    std::cout << "[TcpServerPolicyTest] 用例3：大数值稳定性\n";

    const size_t maxConn = static_cast<size_t>(1) << 20; // 约 100 万
    check(!TcpServer::shouldRejectNewConnection(maxConn - 1, maxConn), "max-1 不应拒绝");
    check(TcpServer::shouldRejectNewConnection(maxConn, maxConn), "max 应拒绝");
}

void testNormalizeIoThreads() {
    std::cout << "[TcpServerPolicyTest] 用例4：IO 线程数归一化\n";

    check(TcpServer::normalizeIoThreadCount(4, 16) == 4, "显式配置线程数应优先采用");
    check(TcpServer::normalizeIoThreadCount(0, 8) == 8, "配置<=0时应回退硬件并发数");
    check(TcpServer::normalizeIoThreadCount(-1, 2) == 2, "负值配置应回退硬件并发数");
    check(TcpServer::normalizeIoThreadCount(0, 0) == 1, "硬件并发为0时应至少返回1");
}

void testDefaultOptions() {
    std::cout << "[TcpServerPolicyTest] 用例5：默认配置值\n";

    TcpServer::Options options;
    check(options.listenIp == "127.0.0.1", "默认监听IP应为 127.0.0.1");
    check(options.listenPort == 8888, "默认端口应为 8888");
    check(options.ioThreads == 0, "默认线程配置应为 0（自动）");
    check(options.maxConnections == 10000, "默认最大连接数应为 10000");
}

} // namespace

int main() {
    std::cout << "[TcpServerPolicyTest] 开始执行\n";

    testRejectBoundary();
    testSmallLimit();
    testLargeValue();
    testNormalizeIoThreads();
    testDefaultOptions();

    std::cout << "[TcpServerPolicyTest] 全部通过\n";
    return 0;
}
