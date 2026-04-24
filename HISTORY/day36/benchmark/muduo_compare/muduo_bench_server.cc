/**
 * muduo_bench_server.cc — muduo 框架的对照基准服务器
 *
 * 使用 muduo::net::HttpServer，路由 GET / → 200 OK，body 与本项目 bench_server 完全相同。
 * 与本项目 bench_server 配置对齐：相同 IO 线程数、相同响应体、相同端口（外部映射）。
 *
 * 用法：
 *   ./muduo_bench_server [port=9090] [io_threads=4]
 *
 * 编译需要：muduo, boost
 */

#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/http/HttpRequest.h>
#include <muduo/net/http/HttpResponse.h>
#include <muduo/net/http/HttpServer.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace muduo;
using namespace muduo::net;

namespace {
const std::string kBody = "hello\n";

void onRequest(const HttpRequest &req, HttpResponse *resp) {
    if (req.path() == "/") {
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setContentType("text/plain");
        resp->setBody(kBody);
    } else {
        resp->setStatusCode(HttpResponse::k404NotFound);
        resp->setStatusMessage("Not Found");
        resp->setCloseConnection(true);
    }
}
} // namespace

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 9090;
    int ioThreads = (argc > 2) ? std::atoi(argv[2]) : 4;

    EventLoop loop;
    HttpServer server(&loop, InetAddress(static_cast<uint16_t>(port)), "muduo-bench");
    server.setHttpCallback(onRequest);
    server.setThreadNum(ioThreads);

    std::printf("[muduo_bench_server] muduo HttpServer listening on 0.0.0.0:%d io_threads=%d\n",
                port, ioThreads);
    server.start();
    loop.loop();
    return 0;
}
