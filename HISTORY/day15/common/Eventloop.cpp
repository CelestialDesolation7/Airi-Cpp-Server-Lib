#include "EventLoop.h"
#include "Channel.h"
#include "Epoll.h"
#include "util.h"
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#ifndef __APPLE__
#include <sys/eventfd.h>
#endif

Eventloop::Eventloop() : ep_(nullptr), quit_(false) {
    ep_ = new Epoll();
#ifdef __APPLE__
    ErrIf(pipe(wakeupFd_) == -1, "pipe create error in Eventloop.");
    fcntl(wakeupFd_[0], F_SETFL, O_NONBLOCK);
    fcntl(wakeupFd_[1], F_SETFL, O_NONBLOCK);
    evtfd_ = wakeupFd_[0];
#else
    evtfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ErrIf(evtfd_ == -1, "eventfd create error in Eventloop.");
#endif
    evtChannel_ = new Channel(this, evtfd_);
    evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));
    evtChannel_->enableReading();
}

Eventloop::~Eventloop() {
    delete ep_;
    delete evtChannel_;
#ifdef __APPLE__
    close(wakeupFd_[0]);
    close(wakeupFd_[1]);
#endif
}

void Eventloop::handleWakeup() {
    char buf[8];
    (void)read(evtfd_, buf, sizeof(buf));
}

void Eventloop::wakeup() {
#ifdef __APPLE__
    char one = 1;
    (void)write(wakeupFd_[1], &one, sizeof(one));
#else
    int i = 1;
    (void)write(evtfd_, &i, sizeof(i));
#endif
}

void Eventloop::loop() {
    while (!quit_) {
        std::vector<Channel *> channels;
        channels = ep_->poll(); // 这里会返回
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            (*it)->handleEvent();
        }
        doPendingFunctors();
    }
}

void Eventloop::queueInLoop(std::function<void()> func) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(func);
    }
    wakeup();
}

void Eventloop::doPendingFunctors() {
    std::vector<std::function<void()>> functors;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (const auto &func : functors) {
        func();
    }
}

void Eventloop::updateChannel(Channel *ch) {
    ep_->updateChannel(ch);
    // 以后要更新channel走eventloop中转
}

void Eventloop::setQuit() { this->quit_ = true; }