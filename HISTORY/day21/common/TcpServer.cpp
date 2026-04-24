#include "TcpServer.h"
#include "Acceptor.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Exception.h"
#include "ThreadPool.h"

#include <functional>
#include <iostream>
#include <thread>

TcpServer::TcpServer() {
    mainReactor_ = std::make_unique<Eventloop>();

    acceptor_ = std::make_unique<Acceptor>(mainReactor_.get());
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1));

    int threadNum = static_cast<int>(std::thread::hardware_concurrency());
    threadPool_ = std::make_unique<ThreadPool>(threadNum);

    for (int i = 0; i < threadNum; ++i)
        subReactors_.push_back(std::make_unique<Eventloop>());

    std::cout << "[TcpServer] Main Reactor + " << threadNum << " Sub Reactors ready." << std::endl;
}

void TcpServer::Start() {
    // 启动所有 subReactor 线程
    for (auto &sub : subReactors_)
        threadPool_->add(std::bind(&Eventloop::loop, sub.get()));

    // 主线程进入 main reactor 循环（阻塞直到 setQuit）
    mainReactor_->loop();
}

void TcpServer::newConnection(int fd) {
    if (fd == -1)
        throw Exception(ExceptionType::INVALID_SOCKET, "newConnection: invalid fd");

    int idx = fd % static_cast<int>(subReactors_.size());
    auto conn = std::make_unique<Connection>(fd, subReactors_[idx].get());

    // 先设置所有回调，再启用 Channel，避免回调尚未就绪就触发事件
    conn->setOnMessageCallback(onMessageCallback_);
    conn->setDeleteConnectionCallback(
        std::bind(&TcpServer::deleteConnection, this, std::placeholders::_1));

    Connection *rawConn = conn.get();
    connections_[fd] = std::move(conn); // unique_ptr 转移进 map

    if (newConnectCallback_)
        newConnectCallback_(rawConn);

    // 通过 queueInLoop 在子 reactor 线程启用 Channel，
    // 确保所有回调已就绪且 Connection 已进入 map
    subReactors_[idx]->queueInLoop([rawConn]() { rawConn->enableInLoop(); });

    std::cout << "[TcpServer] new connection fd=" << fd << std::endl;
}

void TcpServer::deleteConnection(int fd) {
    // 在主 reactor 线程操作 connections_ map（线程安全），
    // 但将 Connection 的实际销毁转移到其所属的子 reactor 线程执行，
    // 避免主 reactor 线程析构 Channel 时子 reactor 仍持有其指针（use-after-free）
    mainReactor_->queueInLoop([this, fd]() {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            Eventloop *ioLoop = it->second->getLoop();
            // 将 Connection 所有权从 map 移出
            std::unique_ptr<Connection> conn = std::move(it->second);
            connections_.erase(it);
            std::cout << "[TcpServer] connection fd=" << fd << " deleted." << std::endl;

            // 移交到子 reactor 线程销毁：在 doPendingFunctors() 中执行，
            // 此时当前 poll() 返回的所有事件已处理完毕，Channel* 不再被引用。
            // 用 shared_ptr 包装 unique_ptr 以满足 std::function 对可复制性的要求，
            // 无需手动 release() + delete，析构由 shared_ptr 引用计数自动触发。
            std::shared_ptr<Connection> guard(std::move(conn));
            ioLoop->queueInLoop([guard]() {
                // guard 在此作用域结束时析构，引用计数归零，Connection 被安全释放
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
    // 先令所有子 Reactor 退出 loop()，使工作线程从 kevent/epoll_wait 返回
    // 回到 ThreadPool 的条件变量等待，这样 threadPool_ 析构时 join() 才不会永久阻塞
    for (auto &sub : subReactors_) {
        sub->setQuit();
        sub->wakeup();
    }
    // 令主 Reactor 退出 loop()，Start() 函数得以返回
    mainReactor_->setQuit();
    mainReactor_->wakeup();
}

TcpServer::~TcpServer() {
    // 析构前确保所有 Reactor 已退出，防止 threadPool_ join() 时
    // 工作线程仍阻塞在 kevent/epoll_wait 内而造成死锁
    stop();
}