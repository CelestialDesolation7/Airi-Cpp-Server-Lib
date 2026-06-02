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
    const int      myId    = node_.getId();
    const uint64_t myTerm  = node_.getCurrentTerm();
    const char    *myState = (node_.getState() == raft::State::Leader)    ? "Leader"
                           : (node_.getState() == raft::State::Candidate) ? "Candidate"
                                                                          : "Follower";

    if (!node_.isLeader()) {
        int         lid = node_.getLeaderId();
        std::string url = leaderUrl(path);
        if (!url.empty()) {
            resp->setStatus(HttpResponse::StatusCode::k307TemporaryRedirect, "Temporary Redirect");
            resp->addHeader("Location", url);
            resp->setContentType("application/json");
            json j = {{"ok", false},
                      {"status", "redirect"},
                      {"reason", "本节点非 Leader，已 307 重定向到 Leader 节点"},
                      {"fromNode", myId},
                      {"fromState", myState},
                      {"term", myTerm},
                      {"leader", lid},
                      {"leaderUrl", url},
                      {"hint", "curl -L 会自动跟随；浏览器 fetch 默认也会跟随"}};
            resp->setBody(j.dump());
        } else {
            resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable, "No Leader");
            resp->setContentType("application/json");
            json j = {{"ok", false},
                      {"status", "no_leader"},
                      {"reason", "集群当前无 Leader（可能正在选举或多数派失联）"},
                      {"fromNode", myId},
                      {"fromState", myState},
                      {"term", myTerm},
                      {"hint", "稍候重试，或检查是否有 ≥ quorum 个节点存活"}};
            resp->setBody(j.dump());
        }
        return; // 同步路径，resp 由 HttpServer 正常发送
    }

    // ── Leader 路径：发 chunked 头 + "accepted" 首块 ──────────────────────────
    const int      peerCount   = node_.getPeerCount();        // 含自己
    const int      quorum      = node_.getQuorum();
    const uint64_t commitBefore= node_.getCommitIndex();
    const uint64_t lastLogBefore = node_.getLastLogIndex();

    std::string hdrs = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/x-ndjson\r\n"
                       "Transfer-Encoding: chunked\r\n";
    for (const auto &[k, v] : resp->headers()) {
        hdrs += k + ": " + v + "\r\n";
    }
    hdrs += "Connection: keep-alive\r\n\r\n";

    json accepted = {
        {"status", "accepted"},
        {"phase", "1/2 leader 已接受请求，开始 Raft 复制"},
        {"leader", myId},
        {"term", myTerm},
        {"clusterSize", peerCount},
        {"quorum", quorum},
        {"commitIndexBefore", commitBefore},
        {"lastLogIndexBefore", lastLogBefore},
        {"cmd", cmd},
        {"timeline",
         json::array({"client → Leader(Node " + std::to_string(myId) + ")",
                      "Leader: append to local log",
                      "Leader: AppendEntries → " + std::to_string(peerCount - 1) + " followers",
                      "Followers ack → leader.matchIndex++",
                      "matchIndex 多数派 ≥ N → commitIndex 推进",
                      "applyCallback → KvStateMachine",
                      "返回 applied 帧给 client"})}};
    conn->send(hdrs + makeChunk(accepted.dump() + "\n"));
    resp->setDeferred(true);

    auto alive = conn->aliveFlag();
    auto *loop = conn->getLoop();
    auto  t0   = std::chrono::steady_clock::now();

    node_.proposeAndNotify(cmd,
        [this, alive, loop, conn, myId, myTerm, peerCount, quorum, commitBefore, t0]
        (bool ok, uint64_t logIndex) {
            // proposeAndNotify 回调在 Raft 线程触发，queueInLoop 把任务投递到
            // conn 的归属 sub-reactor 线程，确保 conn->send() 线程安全
            loop->queueInLoop([this, alive, conn, ok, myId, myTerm, peerCount, quorum,
                               commitBefore, t0, logIndex]() {
                if (auto f = alive.lock(); !f || !*f)
                    return; // 连接已关闭，放弃发送
                const auto dtMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                json body;
                if (ok) {
                    body = {{"status", "applied"},
                            {"ok", true},
                            {"phase", "2/2 多数派已确认并 apply 到状态机"},
                            {"leader", myId},
                            {"term", myTerm},
                            {"logIndex", logIndex},
                            {"commitIndexBefore", commitBefore},
                            {"commitIndexAfter", (uint64_t)node_.getCommitIndex()},
                            {"lastApplied", (uint64_t)node_.getLastApplied()},
                            {"kvSize", (uint64_t)sm_.size()},
                            {"clusterSize", peerCount},
                            {"quorum", quorum},
                            {"latencyMs", dtMs},
                            {"explain", "端到端耗时 = 本地落盘 + 一轮 RTT 到 quorum + apply"}};
                } else {
                    body = {{"status", "applied"},
                            {"ok", false},
                            {"phase", "失败：在 apply 之前丢失了 Leader 身份"},
                            {"leader", myId},
                            {"term", myTerm},
                            {"logIndex", logIndex},
                            {"latencyMs", dtMs},
                            {"error", "leadership lost — 客户端应重试（新 Leader 会承担）"}};
                }
                conn->send(makeChunk(body.dump() + "\n") + "0\r\n\r\n");
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
                   {"kvSize",      sm_.size()}};
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(j.dump());
        });

    // ── GET /admin/scan  →  返回所有 KV 对（供仪表盘 KV 表格刷新）──────────
    srv_->addRoute(HttpRequest::Method::kGet, "/admin/scan",
        [this](const HttpRequest &, HttpResponse *resp) {
            json pairs = json::array();
            try {
                auto j = json::parse(sm_.serialize());
                for (auto &[k, v] : j.items())
                    pairs.push_back({{"key", k}, {"value", v}});
            } catch (...) {}
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(json{{"ok", true},
                               {"count", pairs.size()},
                               {"pairs", pairs}}.dump());
        });

    // ── GET /kv/:key ─────────────────────────────────────────────────────────
    srv_->addPrefixRoute(HttpRequest::Method::kGet, "/kv/",
        [this](const HttpRequest &req, HttpResponse *resp) {
            std::string key = extractKey(req.url());
            if (key.empty()) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setContentType("application/json");
                resp->setBody(R"({"ok":false,"error":"missing key"})");
                return;
            }
            // 附带读请求的节点元数据，供前端展示「本地读 / 可能脏读」提示
            const int   servedBy = node_.getId();
            const auto  st       = node_.getState();
            const char *stateStr = (st == raft::State::Leader)    ? "Leader"
                                 : (st == raft::State::Candidate) ? "Candidate"
                                                                  : "Follower";
            std::string value;
            if (sm_.get(key, value)) {
                resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
                resp->setContentType("application/json");
                resp->setBody(json{{"ok",           true},
                                   {"key",          key},
                                   {"value",        value},
                                   {"servedBy",     servedBy},
                                   {"servedByState",stateStr},
                                   {"term",         node_.getCurrentTerm()},
                                   {"lastApplied",  node_.getLastApplied()}}.dump());
            } else {
                resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
                resp->setContentType("application/json");
                resp->setBody(json{{"ok",           false},
                                   {"key",          key},
                                   {"error",        "not found"},
                                   {"servedBy",     servedBy},
                                   {"servedByState",stateStr},
                                   {"term",         node_.getCurrentTerm()},
                                   {"lastApplied",  node_.getLastApplied()}}.dump());
            }
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
