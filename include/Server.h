#pragma once

class Eventloop;
class Socket;
class InetAddress;
class Channel;

class Server {
private:
  Eventloop *loop;
  Socket *server_sock;
  InetAddress *server_addr;
  Channel *server_sock_channel;

public:
  Server(Eventloop *_loop);
  ~Server();

  void handleReadEvent();
};