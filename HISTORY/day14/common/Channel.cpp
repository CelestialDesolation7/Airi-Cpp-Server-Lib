#include "Channel.h"
#include "EventLoop.h"
#include <functional>

Channel::Channel(Eventloop *_loop, int _fd)
    : loop_(_loop), fd_(_fd), events_(0), revents_(0), inEpoll_(false) {}

Channel::~Channel() {}

void Channel::enableReading() {
    events_ |= POLLER_READ;
    loop_->updateChannel(this);
}

void Channel::disableReading() {
    events_ &= ~POLLER_READ;
    loop_->updateChannel(this);
}

void Channel::enableET() {
    events_ |= POLLER_ET;
    loop_->updateChannel(this);
}

void Channel::disableET() { events_ &= ~POLLER_ET; }

void Channel::disableWriting() {
    events_ &= ~POLLER_WRITE;
    loop_->updateChannel(this);
}

void Channel::enableWriting() {
    events_ |= POLLER_WRITE;
    loop_->updateChannel(this);
}

void Channel::disableAll() {
    events_ = 0;
    loop_->updateChannel(this);
}

bool Channel::isWriting() { return events_ & POLLER_WRITE; }
int Channel::getFd() { return fd_; }

uint32_t Channel::getEvents() { return events_; }

uint32_t Channel::getRevents() { return revents_; }

bool Channel::getInEpoll() { return inEpoll_; }

void Channel::setInEpoll(bool _in) { inEpoll_ = _in; }

void Channel::setRevents(uint32_t _rev) { revents_ = _rev; }

void Channel::handleEvent() {
    if (revents_ & (POLLER_READ | POLLER_PRI)) {
        if (readCallback)
            readCallback();
    }
    if (revents_ & POLLER_WRITE) {
        if (writeCallback)
            writeCallback();
    }
}

void Channel::setReadCallback(std::function<void()> _cb) { readCallback = _cb; }

void Channel::setWriteCallback(std::function<void()> _cb) { writeCallback = _cb; }