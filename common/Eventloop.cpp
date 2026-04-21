#include "EventLoop.h"
#include "Channel.h"
#include "Poller/Poller.h"
#include "util.h"
#include <functional>
#include <mutex>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <sys/eventfd.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#endif

Eventloop::Eventloop() : poller_(nullptr), quit_(false) {
    poller_ = Poller::newDefaultPoller(this); // 用工厂，不 new 具体类

#ifdef __linux__
    evtfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ErrIf(evtfd_ == -1, "eventfd create error.");
    evtChannel_ = new Channel(this, evtfd_);
#elif defined(__APPLE__)
    int pipeFds[2];
    ErrIf(pipe(pipeFds) == -1, "pipe create error.");
    wakeupReadFd_ = pipeFds[0];
    wakeupWriteFd_ = pipeFds[1];
    fcntl(wakeupReadFd_, F_SETFL, fcntl(wakeupReadFd_, F_GETFL) | O_NONBLOCK);
    fcntl(wakeupWriteFd_, F_SETFL, fcntl(wakeupWriteFd_, F_GETFL) | O_NONBLOCK);
    evtChannel_ = new Channel(this, wakeupReadFd_);
#endif

    evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));
    evtChannel_->enableReading();
    // 唤醒 channel 不启用 ET，用 LT，确保每次都能被读到
}

Eventloop::~Eventloop() {
    delete evtChannel_;
    delete poller_;
#ifdef __linux__
    close(evtfd_);
#elif defined(__APPLE__)
    close(wakeupReadFd_);
    close(wakeupWriteFd_);
#endif
}

void Eventloop::handleWakeup() {
#ifdef __linux__
    uint64_t val;
    (void)read(evtfd_, &val, sizeof(val));
#elif defined(__APPLE__)
    char buf[256];
    while (read(wakeupReadFd_, buf, sizeof(buf)) > 0) {
    }
#endif
}

void Eventloop::wakeup() {
#ifdef __linux__
    uint64_t one = 1;
    (void)write(evtfd_, &one, sizeof(one));
#elif defined(__APPLE__)
    char buf = 'w';
    (void)write(wakeupWriteFd_, &buf, 1);
#endif
}

void Eventloop::loop() {
    while (!quit_) {
        std::vector<Channel *> channels = poller_->poll();
        for (auto *ch : channels)
            ch->handleEvent();
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
    for (const auto &func : functors)
        func();
}

void Eventloop::updateChannel(Channel *ch) { poller_->updateChannel(ch); }
void Eventloop::deleteChannel(Channel *ch) { poller_->deleteChannel(ch); }
void Eventloop::setQuit() { quit_ = true; }