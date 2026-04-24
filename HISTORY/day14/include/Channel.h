#pragma once
#include "Macros.h"
#include <cstdint>
#include <functional>

class Epoll;
class Eventloop;

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

#ifdef __APPLE__
constexpr uint32_t POLLER_READ = 1;
constexpr uint32_t POLLER_WRITE = 4;
constexpr uint32_t POLLER_ET = 2;
constexpr uint32_t POLLER_PRI = 8;
#else
constexpr uint32_t POLLER_READ = EPOLLIN;
constexpr uint32_t POLLER_WRITE = EPOLLOUT;
constexpr uint32_t POLLER_ET = EPOLLET;
constexpr uint32_t POLLER_PRI = EPOLLPRI;
#endif

class Channel {
    DISALLOW_COPY_AND_MOVE(Channel)
  private:
    Eventloop *loop_;
    int fd_;
    uint32_t events_;  // 希望监听的事件 (EPOLLIN/EPOLLOUT)
    uint32_t revents_; // 目前正在发生的事件 (returned events)
    bool inEpoll_;     // 标记当前 Channel 是否已经在 Epoll 树上
    std::function<void()> readCallback;
    std::function<void()> writeCallback;

  public:
    Channel(Eventloop *_ep, int _fd);
    ~Channel();

    void handleEvent(); // 处理事件

    void enableET();
    void disableET();

    void enableReading();
    void disableReading();

    void enableWriting();
    void disableWriting();

    void disableAll();
    bool isWriting();

    int getFd();
    uint32_t getEvents();
    uint32_t getRevents();
    bool getInEpoll();
    void setInEpoll(bool _in = true);
    void setRevents(uint32_t _rev);

    void setReadCallback(std::function<void()> _cb);
    void setWriteCallback(std::function<void()> _cb);
};