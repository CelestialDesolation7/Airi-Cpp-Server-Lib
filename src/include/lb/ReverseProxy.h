#pragma once
#include "lb/BackendPool.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

// ReverseProxy：L7 HTTP 反向代理处理器。
//
// 核心流程：
//   pick backend → ++inflight → 阻塞转发（blocking POSIX socket） → --inflight
//   → onSuccess / onFailure（被动健康检查）→ 回写响应
//
// 注意：handle() 在 HttpServer 的 I/O 线程中同步调用。
// 上游连接使用阻塞 socket + SO_RCVTIMEO=500ms，Demo 场景下可接受；
// 生产环境应替换为 EventLoop 异步客户端。

class ReverseProxy {
  public:
    explicit ReverseProxy(BackendPool &pool) : pool_(pool) {}

    // HttpServer::setHttpCallback 的实现目标
    void handle(const HttpRequest &req, HttpResponse *resp);

  private:
    // 向上游发送 HTTP/1.0 请求并读取完整响应（阻塞）
    struct UpstreamResult {
        bool        ok{false};
        int         statusCode{502};
        std::string statusMsg{"Bad Gateway"};
        std::string contentType{"text/plain"};
        std::string body;
    };

    static UpstreamResult callUpstream(const std::string &host, int port,
                                       const HttpRequest &req);

    BackendPool &pool_;
};
