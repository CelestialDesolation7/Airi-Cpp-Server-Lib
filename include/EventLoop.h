#pragma once
#include "Macros.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/types.h>
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
};