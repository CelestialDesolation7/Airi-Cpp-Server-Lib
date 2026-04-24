#ifdef __linux__
#include "Poller/EpollPoller.h"
#include "Channel.h"
#include "Poller/Poller.h"
#include "util.h"
#include <cstddef>
#include <ctime>
#include <unistd.h>
#include <vector>

#define MAX_EVENTS 1024

#include <cerrno>
#include <sys/epoll.h>

EpollPoller::EpollPoller(Eventloop *loop) : Poller(loop), epollFd_(-1), events_(MAX_EVENTS) {
    epollFd_ = epoll_create1(0);
    ErrIf(epollFd_ == -1, "epoll create error");
}

EpollPoller::~EpollPoller() {
    if (epollFd_ != -1) {
        close(epollFd_);
        epollFd_ = -1;
    }
    // events_ 是 std::vector，析构时自动释放
}

void EpollPoller::updateChannel(Channel *channel) {
    int fd = channel->getFd();
    struct epoll_event ev{};
    ev.data.ptr = channel;

    // 原本的 ev.events = channel->getEvents(); 需要在 channel 类全局引入 linux 上的 sys/epoll.h
    if (channel->getListenEvents() & Channel::READ_EVENT)
        ev.events |= (EPOLLIN | EPOLLPRI);
    if (channel->getListenEvents() & Channel::WRITE_EVENT)
        ev.events |= EPOLLOUT;
    if (channel->getListenEvents() & Channel::ET)
        ev.events |= EPOLLET;

    if (!channel->getInEpoll()) {
        ErrIf(epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == -1, "epoll add error");
        channel->setInEpoll();
    } else {
        ErrIf(epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) == -1, "epoll modify error");
    }
}

void EpollPoller::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    ErrIf(epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, NULL) == -1, "epoll delete error");
    channel->setInEpoll(false);
}

std::vector<Channel *> EpollPoller::poll(int timeout) {
    std::vector<Channel *> activeChannels;
    int nfds = epoll_wait(epollFd_, events_.data(), MAX_EVENTS, timeout);
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