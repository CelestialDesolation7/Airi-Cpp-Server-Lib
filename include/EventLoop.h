#pragma once
#include "Macros.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <thread>
#include <vector>

class Poller;
class Channel;

class Eventloop {
    DISALLOW_COPY_AND_MOVE(Eventloop);

  private:
    // unique_ptr 析构顺序 = 声明逆序：evtChannel_ 先析构（从 poller 注销），然后 poller_ 析构
    std::unique_ptr<Poller> poller_;
    std::unique_ptr<Channel> evtChannel_;
    std::atomic<bool> quit_{false};
    std::vector<std::function<void()>> pendingFunctors_;
    std::mutex mutex_;

    // 构造时捕获当前线程 ID，用于 isInLoopThread() 判断。
    // 配合 EventLoopThread 使用后，EventLoop 在其归属线程内构造，
    // tid_ 始终与运行 loop() 的线程一致。
    std::thread::id tid_;

#ifdef __linux__
    int evtfd_{-1};
#elif defined(__APPLE__)
    int wakeupReadFd_{-1};
    int wakeupWriteFd_{-1};
#endif

    void doPendingFunctors();
    void handleWakeup();

  public:
    Eventloop();
    ~Eventloop();
    void loop();
    void setQuit();
    void updateChannel(Channel *ch);
    void deleteChannel(Channel *ch);
    void queueInLoop(std::function<void()> func);
    void wakeup();

    // 判断调用方是否在本 EventLoop 所属的线程中
    bool isInLoopThread() const;

    // 若当前在 EventLoop 线程中，立即执行 func；
    // 否则通过 queueInLoop 跨线程投递，等待下一次 doPendingFunctors() 执行。
    void runInLoop(std::function<void()> func);
};