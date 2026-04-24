// 整个文件仅在 macOS / BSD 上编译，guard 使 clangd 在 Linux 上看到空文件
#ifdef __APPLE__

#include "Poller/KqueuePoller.h"
#include "Channel.h"
#include "util.h"
#include <cerrno>
#include <cstring>
#include <sys/event.h>
#include <unistd.h>
#include <vector>

#define MAX_EVENTS 1024

KqueuePoller::KqueuePoller(Eventloop *loop) : Poller(loop), kqueueFd_(-1), events_(nullptr) {
    kqueueFd_ = kqueue();
    ErrIf(kqueueFd_ == -1, "[server] kqueue create error.");
    events_ = new struct kevent[MAX_EVENTS]; // Bug修复1
    memset(events_, 0, sizeof(struct kevent) * MAX_EVENTS);
}

KqueuePoller::~KqueuePoller() {
    if (kqueueFd_ != -1) {
        close(kqueueFd_);
        kqueueFd_ = -1;
    }
    delete[] events_;
}

void KqueuePoller::updateChannel(Channel *channel) {
    int fd = channel->getFd();
    struct kevent ev[2];
    int n = 0;

    if (channel->getListenEvents() & Channel::READ_EVENT) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, channel);
    }
    if (channel->getListenEvents() & Channel::WRITE_EVENT) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, channel);
    }
    if (n > 0) {
        ErrIf(kevent(kqueueFd_, ev, n, nullptr, 0, nullptr) == -1,
              "[server] kqueue updateChannel error");
    }
    channel->setInEpoll(true);
}

void KqueuePoller::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, ev, 2, nullptr, 0, nullptr); // 忽略错误（某项可能未注册）
    channel->setInEpoll(false);
}

std::vector<Channel *> KqueuePoller::poll(int timeout) {
    std::vector<Channel *> activeChannels;
    struct timespec ts;
    struct timespec *pts = nullptr;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (long)(timeout % 1000) * 1000000;
        pts = &ts;
    }
    int nfds = kevent(kqueueFd_, nullptr, 0, events_, MAX_EVENTS, pts);
    ErrIf(nfds == -1 && errno != EINTR, "[server] kqueue wait error.");

    for (int i = 0; i < nfds; ++i) {
        Channel *ch = static_cast<Channel *>(events_[i].udata);
        int readyEv = 0;
        if (events_[i].filter == EVFILT_READ)
            readyEv |= Channel::READ_EVENT;
        if (events_[i].filter == EVFILT_WRITE)
            readyEv |= Channel::WRITE_EVENT;
        ch->setReadyEvents(readyEv);
        activeChannels.push_back(ch);
    }
    return activeChannels;
}

#endif // __APPLE__