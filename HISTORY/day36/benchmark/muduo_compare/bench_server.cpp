/**
 * bench_server.cpp — Airi-Cpp-Server-Lib 基准服务器（不带任何中间件，仅做 HTTP 回写）
 *
 * 与 demo_server 的区别：
 *   - 移除 RateLimiter（限流会让 QPS 上限变成 50，无法作为吞吐基准）
 *   - 移除 AuthMiddleware（鉴权与本测试无关）
 *   - 日志降到 WARN，避免 IO 干扰
 *   - 只有一个路由 GET /  → 200 OK + 固定 body
 *
 * 这样跟 muduo 的 HttpServer_test.cc 路径长度可比，是公平对照。
 *
 * 用法：
 *   ./bench_server [port=9090] [io_threads=4]
 */

#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
#include <log/Logger.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
std::atomic<HttpServer *> g_server{nullptr};

void onSignal(int) {
    HttpServer *srv = g_server.load();
    if (srv) {
        srv->stop();
    }
}
} // namespace

int main(int argc, char **argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 9090;
    int ioThreads = (argc > 2) ? std::atoi(argv[2]) : 4;

    Logger::setLogLevel(Logger::WARN);

    HttpServer::Options opts;
    opts.tcp.listenIp = "0.0.0.0";
    opts.tcp.listenPort = static_cast<uint16_t>(port);
    opts.tcp.ioThreads = ioThreads;
    opts.tcp.maxConnections = 200000;
    opts.autoClose = true;
    opts.idleTimeoutSec = 120.0;

    HttpServer srv(opts);

    // 只设一个最简单的路由，模拟 muduo HttpServer_test.cc 的 helloworld 风格
    static const std::string kBody = "hello\n";
    srv.addRoute(HttpRequest::Method::kGet, "/", [](const HttpRequest &, HttpResponse *resp) {
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
        resp->setContentType("text/plain");
        resp->setBody(kBody);
    });

    g_server.store(&srv);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "[bench_server] Airi-Cpp-Server-Lib HTTP listening on 0.0.0.0:" << port
              << " io_threads=" << ioThreads << std::endl;

    srv.start();
    return 0;
}
