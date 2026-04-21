#pragma once
#include <cstdint>
#include <functional>

class Epoll;
class Eventloop;

class Channel {
private:
  Eventloop *loop;
  int fd;
  uint32_t events;
  uint32_t revents;
  bool inEpoll;
  std::function<void()> callback;

public:
  Channel(Eventloop *_ep, int _fd);
  ~Channel();

  void handleEvent();
  void enableReading();

  int getFd();
  uint32_t getEvents();
  uint32_t getRevents();
  bool getInEpoll();
  void setInEpoll(bool _in = true);
  void setRevents(uint32_t _rev);

  void setCallback(std::function<void()> _cb);
};
