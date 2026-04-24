#pragma once
#include "Macros.h"
#include <vector>

#ifdef OS_LINUX
#include <sys/epoll.h>
#endif

#ifdef OS_MACOS
#include <sys/event.h>
#endif

class Channel;

class Poller {
    DISALLOW_COPY_AND_MOVE(Poller)
  private:
    int fd_{-1};

#ifdef OS_LINUX
    struct epoll_event *events_{nullptr};
#endif

#ifdef OS_MACOS
    struct kevent *events_{nullptr};
#endif

  public:
    Poller();
    ~Poller();

    void updateChannel(Channel *channel);          // 更新 channel 对应内核中 IO 复用结构体的数据
    void deleteChannel(Channel *channel);          // 删除 channel 对应内核中 IO 复用结构体的数据
    std::vector<Channel *> poll(int timeout = -1); // 从内核获取有事件的 channel 表
};