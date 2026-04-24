#include "Poller.h"
#include "Channel.h"
#include "util.h"
#include <cstddef>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

#define MAX_EVENTS 1024

#ifdef OS_LINUX

#include <cerrno>
#include <sys/epoll.h>

Poller::Poller() : fd_(-1), events_(nullptr) {
    fd_ = epoll_create1(0);
    ErrIf(fd_ == -1, "epoll create error");
    events_ = new epoll_event[MAX_EVENTS];
    memset(events_, 0, sizeof(epoll_event) * MAX_EVENTS);
}

Poller::~Poller() {
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
    delete[] events_;
}

void Poller::updateChannel(Channel *channel) {
    int fd = channel->getFd();
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.ptr = channel;

    // 原本的 ev.events = channel->getEvents(); 需要在 channel 类全局引入 linux 上的 sys/epoll.h
    if (channel->getListenEvents() & Channel::READ_EVENT)
        ev.events |= (EPOLLIN | EPOLLPRI);
    if (channel->getListenEvents() & Channel::WRITE_EVENT)
        ev.events |= EPOLLOUT;
    if (channel->getListenEvents() & Channel::ET)
        ev.events |= EPOLLET;

    if (!channel->getInEpoll()) {
        ErrIf(epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) == -1, "epoll add error");
        channel->setInEpoll();
    } else {
        ErrIf(epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev) == -1, "epoll modify error");
    }
}

void Poller::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    ErrIf(epoll_ctl(fd_, EPOLL_CTL_DEL, fd, NULL) == -1, "epoll delete error");
    channel->setInEpoll(false);
}

std::vector<Channel *> Poller::poll(int timeout) {
    std::vector<Channel *> activeChannels;
    int nfds = epoll_wait(fd_, events_, MAX_EVENTS, timeout);
    ErrIf(nfds == -1 && errno != EINTR, "epoll wait error");

    for (int i = 0; i < nfds; ++i) {
        Channel *ch = static_cast<Channel *>(events_[i].data.ptr);
        int rawEv = events_[i].events;
        // epoll 事件
        int readyEv = 0;
        if (rawEv & (EPOLLIN | EPOLLPRI))
            readyEv |= Channel::READ_EVENT;
        if (rawEv & EPOLLOUT)
            readyEv |= Channel::WRITE_EVENT;
        if (rawEv & EPOLLET)
            readyEv |= Channel::ET;
        ch->setReadyEvents(readyEv);
        activeChannels.push_back(ch);
    }
    return activeChannels;
}

#endif

#ifdef OS_MACOS
#include <cerrno>
#include <sys/event.h>

Poller::Poller() {
    fd_ = kqueue();
    ErrIf(fd_ == -1, "[server] kqueue create error.");
    events_ = new struct kevent[MAX_EVENTS];
    memset(events_, 0, sizeof(struct kevent) * MAX_EVENTS);
}

Poller::~Poller() {
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
    delete[] events_;
}

void Poller::updateChannel(Channel *channel) {
    int fd = channel->getFd();
    struct kevent ev[2];
    int n = 0;

    // kqueue 默认 ET
    if (channel->getListenEvents() & Channel::READ_EVENT) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, channel);
    }
    if (channel->getListenEvents() & Channel::WRITE_EVENT) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, channel);
    }
    if (n > 0) {
        ErrIf(kevent(fd_, ev, n, nullptr, 0, nullptr) == -1, "[server] kqueue updateChannel error");
    }
    channel->setInEpoll(true);
}

void Poller::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    // 不做错误处理。若其中一项未注册，删除不会产生问题
    kevent(fd_, ev, 2, nullptr, 0, nullptr);
    channel->setInEpoll(false);
}

std::vector<Channel *> Poller::poll(int timeout) {
    std::vector<Channel *> activeChannels;
    struct timespec ts;
    struct timespec *pts = nullptr;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (long)(timeout % 1000) * 1000000;
        pts = &ts;
    }
    int nfds = kevent(fd_, nullptr, 0, events_, MAX_EVENTS, pts);
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

#endif