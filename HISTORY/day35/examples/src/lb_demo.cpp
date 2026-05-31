// =============================================================================
//  lb_demo —— L7 反向代理 + 多策略负载均衡演示
//
//  架构：
//    3 个回显后端（:29001 / :29002 / :29003）各跑在独立线程
//    1 个反向代理前端（:28900）接受外部请求，按策略转发
//
//  启动：
//    ./build/examples/lb_demo                     # 默认 RoundRobin
//    ./build/examples/lb_demo --strategy lc       # LeastConnections
//    ./build/examples/lb_demo --strategy ch       # ConsistentHash
//
//  验证：
//    # 轮询：6 次请求均匀落在 3 个后端
//    for i in $(seq 1 6); do curl -s http://localhost:28900/hello; done
//
//    # 会话亲和（一致性哈希）：同 session 总落同一后端
//    for i in $(seq 1 4); do
//      curl -s -H "X-Session-Id: alice" http://localhost:28900/order; done
//
//    # 后端状态
//    curl -s http://localhost:28900/admin/backends | python3 -m json.tool
//
//    # 压测（wrk 需要另行安装）
//    wrk -t2 -c20 -d10s http://localhost:28900/ping
// =============================================================================

#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/HttpServer.h"
#include "lb/BackendPool.h"
#include "lb/ConsistentHash.h"
#include "lb/LeastConnections.h"
#include "lb/ReverseProxy.h"
#include "lb/RoundRobin.h"
#include "log/Logger.h"
#include "net/SignalHandler.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ── 回显后端工厂 ──────────────────────────────────────────────────────────────
// 每个后端是一个普通 HttpServer，返回自己的 ID + 请求路径
static std::thread startBackend(int id, int port) {
    return std::thread([id, port] {
        HttpServer::Options opts;
        opts.tcp.listenPort = static_cast<uint16_t>(port);
        opts.tcp.ioThreads  = 1;

        HttpServer srv(opts);
        srv.setHttpCallback([id](const HttpRequest &req, HttpResponse *resp) {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/plain");
            resp->setBody("backend-" + std::to_string(id) + " " + req.url() + "\n");
        });
        srv.start();
    });
}

int main(int argc, char *argv[]) {
    // ── 解析 --strategy 参数 ─────────────────────────────────────────────────
    std::string strategyName = "rr";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--strategy") == 0)
            strategyName = argv[i + 1];
    }

    // ── 选择策略 ─────────────────────────────────────────────────────────────
    std::unique_ptr<LbStrategy> strategy;
    if (strategyName == "lc") {
        strategy = std::make_unique<LeastConnections>();
        std::cout << "[LB] 策略：LeastConnections（最少连接）\n";
    } else if (strategyName == "ch") {
        strategy = std::make_unique<ConsistentHash>();
        std::cout << "[LB] 策略：ConsistentHash（一致性哈希，key = url + X-Session-Id）\n";
    } else {
        strategy = std::make_unique<RoundRobin>();
        std::cout << "[LB] 策略：RoundRobin（轮询）\n";
    }

    // ── 建立后端池 ───────────────────────────────────────────────────────────
    BackendPool pool(std::move(strategy));
    pool.addBackend("127.0.0.1", 29001);
    pool.addBackend("127.0.0.1", 29002);
    pool.addBackend("127.0.0.1", 29003);
    pool.build(); // 触发一致性哈希预计算（其他策略此调用为空操作）

    // ── 启动 3 个后端服务（后台线程） ─────────────────────────────────────────
    std::vector<std::thread> backendThreads;
    backendThreads.push_back(startBackend(0, 29001));
    backendThreads.push_back(startBackend(1, 29002));
    backendThreads.push_back(startBackend(2, 29003));

    // 给后端一点时间完成监听
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── 反向代理前端 ─────────────────────────────────────────────────────────
    ReverseProxy proxy(pool);

    HttpServer::Options proxyOpts;
    proxyOpts.tcp.listenPort = 28900;
    proxyOpts.tcp.ioThreads  = 2;

    HttpServer proxySrv(proxyOpts);

    // /admin/backends：返回后端池 JSON 状态（不经过代理）
    proxySrv.addRoute(
        HttpRequest::Method::kGet, "/admin/backends",
        [&pool](const HttpRequest & /*req*/, HttpResponse *resp) {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(pool.statusJson());
        });

    // 其余所有请求 → 反向代理
    proxySrv.setHttpCallback(
        [&proxy](const HttpRequest &req, HttpResponse *resp) { proxy.handle(req, resp); });

    // ── 优雅关闭 ─────────────────────────────────────────────────────────────
    Signal::signal(SIGINT,  [&proxySrv] { proxySrv.stop(); });
    Signal::signal(SIGTERM, [&proxySrv] { proxySrv.stop(); });

    std::cout << "[LB] 反向代理监听 :28900，后端 :29001/:29002/:29003\n"
              << "[LB] Ctrl+C 退出\n";

    proxySrv.start(); // 阻塞直到 stop()

    for (auto &t : backendThreads)
        t.detach(); // 进程退出时由 OS 回收

    return 0;
}
