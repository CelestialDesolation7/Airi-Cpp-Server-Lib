#pragma once
#include "Macros.h"
#include "TcpServer.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <functional>
#include <memory>

// HttpServer：在 TcpServer 之上封装 HTTP/1.x 协议处理。
//
// 职责划分：
//   TcpServer  ← 负责 accept / IO / 多 Reactor / 连接生命周期
//   HttpServer ← 负责 HTTP 报文解析（HttpContext）、路由分发（HttpCallback）
//
// 使用方式：
//   HttpServer srv;
//   srv.setHttpCallback([](const HttpRequest& req, HttpResponse* resp) {
//       resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
//       resp->setContentType("text/plain");
//       resp->setBody("Hello World\n");
//   });
//   srv.start();
class Connection;

class HttpServer {
  public:
    DISALLOW_COPY_AND_MOVE(HttpServer)

    // callback 类型：用户处理函数签名，收到一个完整 HttpRequest，填写 HttpResponse
    using HttpCallback = std::function<void(const HttpRequest &, HttpResponse *)>;

    explicit HttpServer();
    ~HttpServer() = default;

    // 设置业务回调（未设置时返回 404）
    void setHttpCallback(HttpCallback cb) { httpCallback_ = std::move(cb); }

    // 启动服务器（阻塞，直到 stop() 被调用）
    void start();
    void stop();

  private:
    void onNewConnection(Connection *conn);
    void onMessage(Connection *conn);
    void onRequest(Connection *conn, const HttpRequest &req);

    // 默认响应：404 Not Found
    void defaultCallback(const HttpRequest &req, HttpResponse *resp);

    std::unique_ptr<TcpServer> server_;
    HttpCallback httpCallback_;
};
