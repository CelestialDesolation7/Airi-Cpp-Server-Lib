#pragma once
#include "Macros.h"
#include <vector>

class Channel;
class Eventloop;

class Poller {
    DISALLOW_COPY_AND_MOVE(Poller)
  protected:
    Eventloop *ownerLoop_;

  public:
    explicit Poller(Eventloop *loop) : ownerLoop_(loop) {}

    virtual ~Poller() = default;

    virtual void updateChannel(Channel *channel) = 0;
    virtual void deleteChannel(Channel *channel) = 0;
    virtual std::vector<Channel *> poll(int timeout = -1) = 0;

    // 静态工厂方法
    static Poller *newDefaultPoller(Eventloop *loop);
};