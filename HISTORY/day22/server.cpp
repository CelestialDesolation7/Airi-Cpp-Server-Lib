#include "include/Connection.h"
#include "include/SignalHandler.h"
#include "include/TcpServer.h"
#include <atomic>
#include <iostream>
#include <thread>

int main() {
    TcpServer server;

    Signal::signal(SIGINT, [&] {
        // 用 atomic_flag 保证信号处理函数幂等：
        // 多线程进程中 SIGINT 可能被任意线程接收，防止重入导致二次析构
        static std::atomic_flag fired = ATOMIC_FLAG_INIT;
        if (fired.test_and_set())
            return;
        std::cout << "[server] Caught SIGINT, shutting down." << std::endl;
        // 只调 stop()，令所有 Reactor 退出循环，Start() 随后返回
        // 不在信号处理函数里调 delete / exit，避免异步信号安全问题
        server.stop();
    });

    server.newConnect([](Connection *conn) {
        std::cout << "[server] New client connected, fd=" << conn->getSocket()->getFd()
                  << std::endl;
    });

    server.onMessage([](Connection *conn) {
        if (conn->getState() != Connection::State::kConnected)
            return;
        std::string msg = conn->getInputBuffer()->retrieveAllAsString();
        if (!msg.empty()) {
            std::cout << "[Thread " << std::this_thread::get_id() << "] recv: " << msg << std::endl;
            conn->send(msg);
        }
    });

    server.Start(); // 阻塞，直到 stop() 被调用

    // Start() 返回后 server 在此析构（栈对象，RAII 自动清理）
    return 0;
}