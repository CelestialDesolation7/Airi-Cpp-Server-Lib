#include "Epoll.h"
#include "Channel.h"
#include "util.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>

#define MAX_EVENTS 1024

#ifdef __APPLE__

Epoll::Epoll() : epfd_(-1), events_(nullptr) {
    epfd_ = kqueue();
    ErrIf(epfd_ == -1, "kqueue create error");
    events_ = new struct kevent[MAX_EVENTS];
    memset(events_, 0, sizeof(struct kevent) * MAX_EVENTS);
}

Epoll::~Epoll() {
    if (epfd_ != -1) {
        close(epfd_);
        epfd_ = -1;
    }
    delete[] events_;
}

void Epoll::updateChannel(Channel *channel) {
    int fd = channel->getFd();
    struct kevent changes[2];
    int nchanges = 0;

    uint32_t ev = channel->getEvents();

    if (ev & POLLER_READ) {
        EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void *)channel);
    }
    if (ev & POLLER_WRITE) {
        EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, (void *)channel);
    } else {
        EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }

    for (int i = 0; i < nchanges; ++i) {
        kevent(epfd_, &changes[i], 1, nullptr, 0, nullptr);
    }
    channel->setInEpoll();
}

void Epoll::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(epfd_, &changes[0], 1, nullptr, 0, nullptr);
    kevent(epfd_, &changes[1], 1, nullptr, 0, nullptr);
    channel->setInEpoll(false);
}

std::vector<Channel *> Epoll::poll(int timeout) {
    struct timespec ts;
    struct timespec *tsp = nullptr;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000L;
        tsp = &ts;
    }
    int nfds = kevent(epfd_, nullptr, 0, events_, MAX_EVENTS, tsp);
    ErrIf(nfds == -1 && errno != EINTR, "kevent wait error");

    std::vector<Channel *> activeChannels;
    for (int i = 0; i < nfds; ++i) {
        Channel *ch = (Channel *)events_[i].udata;
        uint32_t revt = 0;
        if (events_[i].filter == EVFILT_READ)
            revt |= POLLER_READ;
        if (events_[i].filter == EVFILT_WRITE)
            revt |= POLLER_WRITE;
        ch->setRevents(ch->getRevents() | revt);
        bool found = false;
        for (auto *c : activeChannels) {
            if (c == ch) {
                found = true;
                break;
            }
        }
        if (!found)
            activeChannels.push_back(ch);
    }
    return activeChannels;
}

#else

Epoll::Epoll() : epfd_(-1), events_(nullptr) {
    epfd_ = epoll_create1(0);
    ErrIf(epfd_ == -1, "epoll create error");
    events_ = new epoll_event[MAX_EVENTS];
    bzero(events_, sizeof(epoll_event) * MAX_EVENTS);
}

Epoll::~Epoll() {
    if (epfd_ != -1) {
        close(epfd_);
        epfd_ = -1;
    }
    delete[] events_;
}

void Epoll::updateChannel(Channel *channel) {
    int fd = channel->getFd();
    struct epoll_event ev;

    ev.data.ptr = channel;
    ev.events = channel->getEvents();

    if (!channel->getInEpoll()) {
        ErrIf(epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == -1, "epoll add error");
        channel->setInEpoll();
    } else {
        ErrIf(epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1, "epoll modify error");
    }
}

void Epoll::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    ErrIf(epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, NULL) == -1, "epoll delete error");
    channel->setInEpoll(false);
}

std::vector<Channel *> Epoll::poll(int timeout) {
    std::vector<Channel *> activeChannels;
    int nfds = epoll_wait(epfd_, events_, MAX_EVENTS, timeout);
    ErrIf(nfds == -1 && errno != EINTR, "epoll wait error");

    for (int i = 0; i < nfds; ++i) {
        Channel *ch = (Channel *)events_[i].data.ptr;
        ch->setRevents(events_[i].events);
        activeChannels.push_back(ch);
    }
    return activeChannels;
}

#endif