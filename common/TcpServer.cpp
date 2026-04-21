#include "TcpServer.h"
#include "Acceptor.h"
#include "Connection.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "Exception.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <thread>

TcpServer::TcpServer() {
    mainReactor_ = std::make_unique<Eventloop>();

    acceptor_ = std::make_unique<Acceptor>(mainReactor_.get());
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1));

    // 至少 1 个 IO 线程；hardware_concurrency() 可能返回 0
    int threadNum = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    threadPool_ = std::make_unique<EventLoopThreadPool>(mainReactor_.get());
    threadPool_->setThreadNums(threadNum);

    std::cout << "[TcpServer] Main Reactor + " << threadNum << " Sub Reactors ready." << std::endl;
}

void TcpServer::Start() {
    // 1. 启动所有 IO 线程：每个线程在内部构造 EventLoop 并进入 loop()
    //    start() 内部同步等待每个 EventLoop 就绪后才返回，
    //    此后 nextLoop() 可安全使用
    threadPool_->start();

    // 2. 主线程进入 main-reactor 循环（阻塞直到 stop()）
    mainReactor_->loop();
}

void TcpServer::newConnection(int fd) {
    if (fd == -1)
        throw Exception(ExceptionType::INVALID_SOCKET, "newConnection: invalid fd");

    // 轮询选择一个 sub-reactor 归属这条连接
    Eventloop *subLoop = threadPool_->nextLoop();
    auto conn = std::make_unique<Connection>(fd, subLoop);

    // 先注入所有回调，再通过 queueInLoop 在归属 sub-reactor 线程启用 Channel，
    // 避免 Channel 就绪触发事件时回调尚未设置
    conn->setOnMessageCallback(onMessageCallback_);
    conn->setDeleteConnectionCallback(
        std::bind(&TcpServer::deleteConnection, this, std::placeholders::_1));

    Connection *rawConn = conn.get();
    connections_[fd] = std::move(conn);

    if (newConnectCallback_)
        newConnectCallback_(rawConn);

    // queueInLoop 跨线程投递：在 sub-reactor 线程启用读事件（enableInLoop），
    // 此时 connections_[fd] 已插入 map，回调均已就绪
    subLoop->queueInLoop([rawConn]() { rawConn->enableInLoop(); });

    std::cout << "[TcpServer] new connection fd=" << fd << std::endl;
}

void TcpServer::deleteConnection(int fd) {
    // 本函数由 sub-reactor 线程经 Connection::close() 回调链调用，
    // 需切换到 main-reactor 线程操作 connections_ map（逻辑隔离，无需 mutex）
    mainReactor_->queueInLoop([this, fd]() {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            Eventloop *ioLoop = it->second->getLoop();
            std::unique_ptr<Connection> conn = std::move(it->second);
            connections_.erase(it);
            std::cout << "[TcpServer] connection fd=" << fd << " deleted." << std::endl;

            // 将 Connection 析构投递到其归属 sub-reactor 线程执行：
            // 保证 Connection::~Connection()（调用 loop_->deleteChannel）
            // 发生在该 sub-reactor 本次 poll() 结束、events_ 遍历完毕之后，
            // 消除 Channel* 悬空野指针风险。
            // shared_ptr 包装 unique_ptr：满足 std::function 对可复制性的要求，
            // 引用计数归零时自动析构 Connection，无需 release()+delete。
            std::shared_ptr<Connection> guard(std::move(conn));
            ioLoop->queueInLoop([guard]() {
                // guard 在此作用域结束时析构，Connection 被安全释放
            });
        }
    });
}

void TcpServer::onMessage(std::function<void(Connection *)> fn) {
    onMessageCallback_ = std::move(fn);
}
void TcpServer::newConnect(std::function<void(Connection *)> fn) {
    newConnectCallback_ = std::move(fn);
}

void TcpServer::stop() {
    // 通知所有 sub-reactor 退出 loop()
    threadPool_->stopAll();
    // 通知 main-reactor 退出 loop()，使 Start() 函数得以返回
    mainReactor_->setQuit();
    mainReactor_->wakeup();
}

TcpServer::~TcpServer() {
    // 1. 令所有 reactor 退出 loop()（幂等，重复调用无害）
    stop();
    // 2. 显式等待所有 IO 线程实际退出（join），
    //    之后 connections_ 析构时调用 loop->deleteChannel() 才没有竞态风险
    threadPool_->joinAll();
    // 3. 成员按声明逆序自动析构（详见 TcpServer.h 注释）
}
