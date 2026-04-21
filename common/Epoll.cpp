#include "Epoll.h"
#include "util.h"
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <vector>

#define MAX_EVENTS 1024

#ifdef __APPLE__

Epoll::Epoll() : epfd(-1), events(nullptr) {
  epfd = kqueue();
  errif(epfd == -1, "kqueue create error");
  events = new struct kevent[MAX_EVENTS];
  memset(events, 0, sizeof(struct kevent) * MAX_EVENTS);
}

Epoll::~Epoll() {
  if (epfd != -1) {
    close(epfd);
    epfd = -1;
  }
  delete[] events;
}

void Epoll::addFd(int fd, uint32_t op) {
  struct kevent change;
  (void)op; // flags are implicit: always READ + EV_CLEAR (edge-triggered)
  EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  errif(kevent(epfd, &change, 1, nullptr, 0, nullptr) == -1,
        "kqueue add event error");
}

std::vector<ActiveEvent> Epoll::poll(int timeout) {
  struct timespec ts;
  struct timespec *tsp = nullptr;
  if (timeout >= 0) {
    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (timeout % 1000) * 1000000L;
    tsp = &ts;
  }
  int nfds = kevent(epfd, nullptr, 0, events, MAX_EVENTS, tsp);
  errif(nfds == -1, "kevent wait error");

  std::vector<ActiveEvent> active_events;
  for (int i = 0; i < nfds; ++i) {
    ActiveEvent ae;
    ae.fd = static_cast<int>(events[i].ident);
    ae.events = POLLER_READ;
    active_events.push_back(ae);
  }
  return active_events;
}

#else // Linux epoll

Epoll::Epoll() : epfd(-1), events(nullptr) {
  epfd = epoll_create1(0);
  errif(epfd == -1, "epoll create error");
  events = new epoll_event[MAX_EVENTS];
  bzero(events, sizeof(epoll_event) * MAX_EVENTS);
}

Epoll::~Epoll() {
  if (epfd != -1) {
    close(epfd);
    epfd = -1;
  }
  delete[] events;
}

void Epoll::addFd(int fd, uint32_t op) {
  struct epoll_event ev;
  bzero(&ev, sizeof(ev));
  ev.data.fd = fd;
  ev.events = op;
  errif(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev), "epoll add event error");
}

std::vector<ActiveEvent> Epoll::poll(int timeout) {
  std::vector<ActiveEvent> active_events;
  int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
  errif(nfds == -1, "epoll wait error");

  for (int i = 0; i < nfds; ++i) {
    ActiveEvent ae;
    ae.fd = events[i].data.fd;
    ae.events = events[i].events;
    active_events.push_back(ae);
  }
  return active_events;
}

#endif
