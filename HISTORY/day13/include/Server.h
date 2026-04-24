#pragma once

#include <map>
#include <vector>

class Connection;
class InetAddress;
class Eventloop;
class Socket;
class Acceptor;
class ThreadPool;

class Server {
  private:
    Eventloop *mainReactor;
    Acceptor *acceptor;
    // 保存所有的连接
    // 我们的服务器终于封装了几乎所有系统细节，变得符合直觉
    std::map<int, Connection *> connection;
    ThreadPool *threadPool;
    std::vector<Eventloop *> subReactors;

  public:
    Server(Eventloop *_loop);
    ~Server();

    // 【新增】提供给 Acceptor 的回调函数，当有新连接时调用
    void newConnection(Socket *client_sock, InetAddress *client_addr);
    void deleteConnection(Socket *sock);
};