#include "SignalHandler.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/HttpServer.h"
#include "log/Logger.h"
#include <atomic>
#include <iostream>
#include <sstream>

// ── HTTP 请求路由处理 ─────────────────────────────────────────────────────────
static void handleRequest(const HttpRequest &req, HttpResponse *resp) {
    const std::string &url = req.url();

    // GET /
    if (req.method() == HttpRequest::Method::kGet && url == "/") {
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
        resp->setContentType("text/html; charset=utf-8");
        resp->setBody(
            "<html><head><title>Airi-Cpp-Server-Lib</title></head>"
            "<body><h1>Hello from Airi-Cpp-Server-Lib!</h1>"
            "<p>This is a minimal HTTP/1.1 server built on a custom C++ Reactor network "
            "library.</p>"
            "<ul>"
            "<li><a href=\"/hello\">/hello</a> - plain text greeting</li>"
            "<li><a href=\"/echo\">/echo</a> - POST body echo</li>"
            "<li><a href=\"/query?name=world&lang=cpp\">/query</a> - URL query params demo</li>"
            "</ul></body></html>\n");
        return;
    }

    // GET /hello
    if (req.method() == HttpRequest::Method::kGet && url == "/hello") {
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
        resp->setContentType("text/plain; charset=utf-8");
        resp->setBody("Hello, World!\n");
        return;
    }

    // POST /echo  → 原样返回请求体
    if (req.method() == HttpRequest::Method::kPost && url == "/echo") {
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
        resp->setContentType("text/plain; charset=utf-8");
        resp->setBody(req.body());
        return;
    }

    // GET /query  → 打印 URL 查询参数
    if (req.method() == HttpRequest::Method::kGet && url == "/query") {
        std::ostringstream oss;
        oss << "URL query parameters:\n";
        for (const auto &kv : req.queryParams()) {
            oss << "  " << kv.first << " = " << kv.second << "\n";
        }
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
        resp->setContentType("text/plain; charset=utf-8");
        resp->setBody(oss.str());
        return;
    }

    // 其他路径：404
    resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
    resp->setContentType("text/plain");
    resp->setCloseConnection(true);
    resp->setBody("404 Not Found\n");
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    Logger::setLogLevel(Logger::INFO);

    HttpServer srv;
    srv.setHttpCallback(handleRequest);

    Signal::signal(SIGINT, [&] {
        static std::atomic_flag fired = ATOMIC_FLAG_INIT;
        if (fired.test_and_set())
            return;
        std::cout << "\n[http_server] Caught SIGINT, shutting down.\n";
        srv.stop();
    });

    std::cout << "[http_server] Listening on port 8888. "
                 "Visit http://127.0.0.1:8888/\n";
    srv.start();
    return 0;
}
