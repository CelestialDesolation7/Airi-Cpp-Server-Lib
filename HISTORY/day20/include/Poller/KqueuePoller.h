#pragma once

// 整个文件仅在 macOS / BSD 上有意义，用 guard 避免 clangd 在 Linux 上报 fatal 错误
#ifdef __APPLE__

#include "Poller.h"
#include <sys/event.h>

class KqueuePoller : public Poller {
  private:
    int kqueueFd_{-1};
    struct kevent *events_{nullptr};

  public:
    explicit KqueuePoller(Eventloop *loop);
    ~KqueuePoller() override;

    void updateChannel(Channel *channel) override;
    void deleteChannel(Channel *channel) override;
    std::vector<Channel *> poll(int timeout = -1) override;
};

#endif // __APPLE__