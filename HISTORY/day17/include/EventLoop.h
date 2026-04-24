#pragma once
#include "Macros.h"
#include <functional>
#include <mutex>
#include <sys/types.h>
#include <vector>

class Poller;
class Channel;

class Eventloop {
    DISALLOW_COPY_AND_MOVE(Eventloop);

  private:
    Poller *poller_{nullptr};
    Channel *evtChannel_{nullptr};
    bool quit_{false};
    std::vector<std::function<void()>> pendingFunctors_;
    std::mutex mutex_;

#ifdef OS_LINUX
    int evtfd_;
#endif

#ifdef OS_MACOS
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
    void queueInLoop(std::function<void()> func);
    void wakeup();
};