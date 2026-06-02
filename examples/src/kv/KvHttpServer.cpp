#include "kv/KvHttpServer.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/StaticFileHandler.h"
#include "log/Logger.h"
#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── 分块帧格式（HTTP/1.1 Transfer-Encoding: chunked）──────────────────────────
// 返回 "hex-size\r\ndata\r\n"
static std::string makeChunk(std::string_view data) {
    char hdr[32];
    int n = std::snprintf(hdr, sizeof(hdr), "%zx\r\n", data.size());
    std::string chunk;
    chunk.reserve(static_cast<size_t>(n) + data.size() + 2);
    chunk.append(hdr, static_cast<size_t>(n));
    chunk.append(data);
    chunk.append("\r\n");
    return chunk;
}

// ── 构造：用 httpPort 初始化内嵌 HttpServer ──────────────────────────────────
KvHttpServer::KvHttpServer(raft::RaftNode &node, KvStateMachine &sm,
                           uint16_t httpPort, uint16_t baseHttpPort,
                           std::string staticDir)
    : node_(node), sm_(sm), baseHttpPort_(baseHttpPort),
      staticDir_(std::move(staticDir)) {
    HttpServer::Options opts;
    opts.tcp.listenPort = httpPort;
    srv_ = std::make_unique<HttpServer>(opts);
}

// ── 工具：从 /kv/<key> 提取 key ─────────────────────────────────────────────
std::string KvHttpServer::extractKey(const std::string &path) {
    constexpr std::string_view prefix = "/kv/";
    if (path.size() > prefix.size())
        return path.substr(prefix.size());
    return {};
}

// ── 工具：生成 Leader 的重定向 URL ──────────────────────────────────────────
std::string KvHttpServer::leaderUrl(const std::string &path) const {
    int lid = node_.getLeaderId();
    if (lid < 0) return {};
    return "http://127.0.0.1:" + std::to_string(baseHttpPort_ + lid) + path;
}

// ── 异步写请求处理（PUT / DEL 共用）─────────────────────────────────────────
//
// 非 Leader：同步返回 307/503，resp 由 HttpServer 正常序列化发送。
// Leader   ：立即通过 conn->send() 发送 HTTP/1.1 Chunked 头 + {"status":"pending"}，
//            标记 resp.setDeferred(true) 跳过 HttpServer 的自动发送；
//            Raft apply 回调通过 loop->queueInLoop() 发送结果块 + 终止块（0\r\n\r\n）。
//            全程无 future::wait / sleep，不阻塞 IO 线程。
void KvHttpServer::handleWriteAsync(const std::string &cmd,
                                    const std::string &path,
                                    const HttpRequest & /*req*/,
                                    HttpResponse *resp,
                                    Connection *conn) {
    const int      myId   = node_.getId();
    const uint64_t myTerm = node_.getCurrentTerm();

    if (!node_.isLeader()) {
        int         lid = node_.getLeaderId();
        std::string url = leaderUrl(path);
        if (!url.empty()) {
            resp->setStatus(HttpResponse::StatusCode::k307TemporaryRedirect, "Temporary Redirect");
            resp->addHeader("Location", url);
            resp->setContentType("application/json");
            // 极简重定向响应：只保留必要的诊断字段
            resp->setBody(R"({"ok":false,"status":"redirect","leader":)" +
                          std::to_string(lid) + R"(,"leaderUrl":")" + url + R"("})");
        } else {
            resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable, "No Leader");
            resp->setContentType("application/json");
            resp->setBody(R"({"ok":false,"status":"no_leader","term":)" +
                          std::to_string(myTerm) + "}");
        }
        return;
    }

    // ── Leader 路径：chunked 响应（首块立即发，尾块在 apply 后发）─────────
    std::string hdrs = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/x-ndjson\r\n"
                       "Transfer-Encoding: chunked\r\n";
    for (const auto &[k, v] : resp->headers()) {
        hdrs += k + ": " + v + "\r\n";
    }
    hdrs += "Connection: keep-alive\r\n\r\n";

    // 首块前快照：向前端提供完整的 Raft 状态信息
    const int      clusterSize  = node_.getPeerCount();   // 含自身
    const int      quorum       = node_.getQuorum();
    const uint64_t commitBefore = node_.getCommitIndex();
    const uint64_t lastLogBefore= node_.getLastLogIndex();

    // 首块：告知客户端已接受，附带 Raft 集群元信息供前端渲染
    {
        json j = {
            {"status",            "accepted"},
            {"leader",            myId},
            {"term",              myTerm},
            {"clusterSize",       clusterSize},
            {"quorum",            quorum},
            {"commitIndexBefore", commitBefore},
            {"lastLogIndexBefore",lastLogBefore},
            {"cmd",               cmd}
        };
        conn->send(hdrs + makeChunk(j.dump() + "\n"));
    }
    resp->setDeferred(true);

    auto alive   = conn->aliveFlag();
    auto *loop   = conn->getLoop();
    auto  t0     = std::chrono::steady_clock::now();
    auto *nodePtr= &node_;
    auto *smPtr  = &sm_;

    node_.proposeAndNotify(cmd,
        [alive, loop, conn, myId, myTerm, clusterSize, quorum, commitBefore, nodePtr, smPtr, t0]
        (bool ok, uint64_t logIndex) {
            loop->queueInLoop([alive, conn, ok, logIndex,
                               myId, myTerm, clusterSize, quorum, commitBefore,
                               nodePtr, smPtr, t0]() {
                if (auto f = alive.lock(); !f || !*f) return;
                const double dtMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::string body;
                if (ok) {
                    json j = {
                        {"status",            "applied"},
                        {"ok",                true},
                        {"leader",            myId},
                        {"term",              myTerm},
                        {"logIndex",          logIndex},
                        {"commitIndexBefore", commitBefore},
                        {"commitIndexAfter",  nodePtr->getCommitIndex()},
                        {"lastApplied",       nodePtr->getLastApplied()},
                        {"quorum",            quorum},
                        {"clusterSize",       clusterSize},
                        {"kvSize",            smPtr->size()},
                        {"latencyMs",         dtMs}
                    };
                    body = j.dump() + "\n";
                } else {
                    body = R"({"status":"error","ok":false,"reason":"leadership lost"})" "\n";
                }
                conn->send(makeChunk(body) + "0\r\n\r\n");
            });
        });
}

// ── start：注册路由后启动（阻塞直到 stop()）─────────────────────────────────
void KvHttpServer::start() {
    // ── CORS 中间件（允许仪表盘跨端口查询其他节点）──────────────────────────
    srv_->use([](const HttpRequest &req, HttpResponse *resp,
                 const HttpServer::MiddlewareNext &next) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, PUT, DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
        if (req.method() == HttpRequest::Method::kOptions) {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            return;
        }
        next();
    });

    // ── GET /         →  静态仪表盘页面（从 staticDir_ 读取）────────────────
    srv_->addRoute(HttpRequest::Method::kGet, "/",
        [this](const HttpRequest &req, HttpResponse *resp) {
            StaticFileHandler::Options opts;
            opts.cacheControl = "no-cache, no-store, must-revalidate";
            if (!StaticFileHandler::serve(req, resp, staticDir_ + "/index.html", opts)) {
                resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
                resp->setContentType("text/plain");
                resp->setBody("Dashboard not found.\n"
                              "Start kv_node with --static-dir pointing to examples/static/kv/\n");
            }
        });

    // ── GET /app.js   →  仪表盘 JavaScript ──────────────────────────────────
    srv_->addRoute(HttpRequest::Method::kGet, "/app.js",
        [this](const HttpRequest &req, HttpResponse *resp) {
            StaticFileHandler::Options opts;
            opts.cacheControl = "no-cache, no-store, must-revalidate";
            if (!StaticFileHandler::serve(req, resp, staticDir_ + "/app.js", opts)) {
                resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
                resp->setContentType("text/plain");
                resp->setBody("app.js not found\n");
            }
        });

    // ── GET /admin/raft  →  节点 Raft 状态（含 kvSize 字段）────────────────
    srv_->addRoute(HttpRequest::Method::kGet, "/admin/raft",
        [this](const HttpRequest &, HttpResponse *resp) {
            const char *s = "Follower";
            if (node_.getState() == raft::State::Leader)    s = "Leader";
            if (node_.getState() == raft::State::Candidate) s = "Candidate";
            json j{{"id",          node_.getId()},
                   {"state",       s},
                   {"term",        node_.getCurrentTerm()},
                   {"leaderId",    node_.getLeaderId()},
                   {"commitIndex", node_.getCommitIndex()},
                   {"lastApplied", node_.getLastApplied()},
                   {"kvSize",      sm_.size()},
                   {"transferTarget",     node_.getTransferTarget()},
                   {"transfersInitiated", node_.getTransfersInitiated()},
                   {"transfersSucceeded", node_.getTransfersSucceeded()}};
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(j.dump());
        });

    // ── POST /admin/transfer?to=<id>  →  主动 Leader Transfer（让贤）──────
    // 仅 Leader 接收；to 可省略，省略时由 RaftNode 自动选 matchIndex 最高的 Follower。
    srv_->addRoute(HttpRequest::Method::kPost, "/admin/transfer",
        [this](const HttpRequest &req, HttpResponse *resp) {
            resp->setContentType("application/json");
            if (!node_.isLeader()) {
                resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable, "Not Leader");
                resp->setBody(R"({"ok":false,"error":"not leader, send to current leader"})");
                return;
            }
            std::string toStr = req.queryParam("to");
            int target = -1;
            if (!toStr.empty()) {
                try { target = std::stoi(toStr); }
                catch (...) {
                    resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                    resp->setBody(R"({"ok":false,"error":"invalid 'to' param"})");
                    return;
                }
            }
            bool started = node_.transferLeadership(target);
            if (!started) {
                resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable, "No Target");
                resp->setBody(R"j({"ok":false,"error":"transfer not started (no target?)"})j");
                return;
            }
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setBody(json{{"ok", true},
                               {"from", node_.getId()},
                               {"requestedTarget", target}}.dump());
        });

    // ── GET /admin/scan  →  返回所有 KV 对（供仪表盘 KV 表格刷新）──────────
    srv_->addRoute(HttpRequest::Method::kGet, "/admin/scan",
        [this](const HttpRequest &, HttpResponse *resp) {
            json pairs = json::array();
            auto kv = sm_.scan();  // 取 map 副本（shared_lock，不阻塞其他读）
            for (const auto &[k, v] : kv)
                pairs.push_back({{"key", k}, {"value", v}});
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(json{{"ok", true},
                               {"count", pairs.size()},
                               {"pairs", pairs}}.dump());
        });

    // ── GET /kv/:key —— 线性一致读（Follower ReadIndex 协议）────────────────
    // 任意节点（Leader 或 Follower）均可处理读请求：
    //   Leader：直接走本地 ReadIndex 协议。
    //   Follower：向 Leader 发 ReadIndex RPC 拿到确认的 readIndex，
    //             等 lastApplied >= readIndex 后读本地状态机（减少一次外部 RTT）。
    // 写请求（PUT/DELETE）仍需 Leader 执行，非 Leader 返回 307。
    srv_->addAsyncPrefixRoute(HttpRequest::Method::kGet, "/kv/",
        [this](const HttpRequest &req, HttpResponse *resp, Connection *conn) {
            std::string key = extractKey(req.url());
            if (key.empty()) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setContentType("application/json");
                resp->setBody(R"({"ok":false,"error":"missing key"})");
                return;
            }
            // 异步路径：所有节点均可参与，等 proposeFollowerRead 完成后再发响应
            resp->setDeferred(true);
            auto alive = conn->aliveFlag();
            auto *loop = conn->getLoop();
            node_.proposeFollowerRead([this, alive, loop, conn, key](bool ok) {
                loop->queueInLoop([this, alive, conn, key, ok]() {
                    if (auto f = alive.lock(); !f || !*f) return;
                    std::string body;
                    if (!ok) {
                        body = R"({"ok":false,"error":"no leader or leadership lost, retry"})";
                        conn->send("HTTP/1.1 503 Service Unavailable\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Content-Length: " + std::to_string(body.size()) +
                                   "\r\n\r\n" + body);
                        return;
                    }
                    std::string value;
                    if (sm_.get(key, value)) {
                        body = R"({"ok":true,"key":")" + key + R"(","value":")" + value + R"("})";
                        conn->send("HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Content-Length: " + std::to_string(body.size()) +
                                   "\r\n\r\n" + body);
                    } else {
                        body = R"({"ok":false,"key":")" + key + R"(","error":"not found"})";
                        conn->send("HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Content-Length: " + std::to_string(body.size()) +
                                   "\r\n\r\n" + body);
                    }
                });
            });
        });

    // ── PUT /kv/:key  (body = value)  ────────────────────────────────────────
    //    异步路由：handler 接收 Connection*，立即发 chunked 头，不阻塞 IO 线程
    srv_->addAsyncPrefixRoute(HttpRequest::Method::kPut, "/kv/",
        [this](const HttpRequest &req, HttpResponse *resp, Connection *conn) {
            std::string key = extractKey(req.url());
            if (key.empty()) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setContentType("application/json");
                resp->setBody(R"({"ok":false,"error":"missing key"})");
                return; // 同步返回，resp 由 HttpServer 发送
            }
            handleWriteAsync("PUT " + key + " " + req.body(), req.url(), req, resp, conn);
        });

    // ── DELETE /kv/:key ──────────────────────────────────────────────────────
    srv_->addAsyncPrefixRoute(HttpRequest::Method::kDelete, "/kv/",
        [this](const HttpRequest &req, HttpResponse *resp, Connection *conn) {
            std::string key = extractKey(req.url());
            if (key.empty()) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setContentType("application/json");
                resp->setBody(R"({"ok":false,"error":"missing key"})");
                return;
            }
            handleWriteAsync("DEL " + key, req.url(), req, resp, conn);
        });

    srv_->start();
}

void KvHttpServer::stop() {
    srv_->stop();
}
