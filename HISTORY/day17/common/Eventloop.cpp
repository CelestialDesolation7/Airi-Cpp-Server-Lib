#include "EventLoop.h"
#include "Channel.h"
#include "Poller.h"
#include "util.h"
#include <functional>
#include <mutex>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#ifdef OS_LINUX
#include <sys/eventfd.h>
#endif

#ifdef OS_MACOS
#include <fcntl.h>
#endif

Eventloop::Eventloop() : poller_(nullptr), quit_(false) {
    poller_ = new Poller(); // 这个是我们自定义的epoll，不是OS给的

#ifdef OS_LINUX
    evtfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ErrIf(evtfd_ == -1, "[server] eventfd create error in Eventloop.");
    evtChannel_ = new Channel(this, evtfd_);
#endif
#ifdef OS_MACOS
    int pipeFds[2];
    ErrIf(pipe(pipeFds) == -1, "[server] pipe create error in Eventloop.");
    wakeupReadFd_ = pipeFds[0];
    wakeupWriteFd_ = pipeFds[1];

    fcntl(wakeupReadFd_, F_SETFL, fcntl(wakeupReadFd_, F_GETFL) | O_NONBLOCK);
    fcntl(wakeupWriteFd_, F_SETFL, fcntl(wakeupWriteFd_, F_GETFL) | O_NONBLOCK);
    evtChannel_ = new Channel(this, wakeupReadFd_);
#endif

    evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));
    evtChannel_->enableReading();
}

Eventloop::~Eventloop() {
    delete evtChannel_;
    delete poller_;
#ifdef OS_LINUX
    close(evtfd_);
#endif
#ifdef OS_MACOS
    close(wakeupReadFd_);
    close(wakeupWriteFd_);
#endif
}

void Eventloop::handleWakeup() {
#ifdef OS_LINUX
    uint64_t i = 1;
    (void)read(evtfd_, &i, sizeof(i));
#endif
#ifdef OS_MACOS
    char buf[256];
    while (read(wakeupReadFd_, buf, sizeof(buf)) > 0) {
    }
#endif
}

void Eventloop::wakeup() {
#ifdef OS_LINUX
    uint64_t i = 1;
    (void)write(evtfd_, &i, sizeof(i));
#endif
#ifdef OS_MACOS
    char buf = 'w';
    (void)write(wakeupWriteFd_, &buf, 1);
#endif
}

void Eventloop::loop() {
    while (!quit_) {
        std::vector<Channel *> channels;
        channels = poller_->poll(); // 这里会返回
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
    poller_->updateChannel(ch);
    // 以后要更新channel走eventloop中转
}

void Eventloop::setQuit() { this->quit_ = true; }