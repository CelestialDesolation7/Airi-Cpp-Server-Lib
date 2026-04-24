#pragma once
#include <cstdint>
#include <vector>

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

// 统一的活跃事件结构
struct ActiveEvent {
  int fd;
  uint32_t events; // POLLIN-like flags
};

// 平台标志常量
#ifdef __APPLE__
constexpr uint32_t POLLER_READ = 1;
constexpr uint32_t POLLER_ET   = 2;
#else
constexpr uint32_t POLLER_READ = EPOLLIN;
constexpr uint32_t POLLER_ET   = EPOLLET;
#endif

class Epoll {
private:
  int epfd;
#ifdef __APPLE__
  struct kevent *events;
#else
  struct epoll_event *events;
#endif

public:
  Epoll();
  ~Epoll();
  void addFd(int fd, uint32_t op);
  std::vector<ActiveEvent> poll(int timeout = -1);
};
