#pragma once
// KvHttpServer —— 把 KvStateMachine + RaftNode 暴露为 REST HTTP 接口
//
// 路由：
//   GET    /kv/:key      本地读（最终一致性）→ 200 + JSON，或 404
//   PUT    /kv/:key      Raft 强一致写（body 为 value）→ 200 Chunked 或 307/503
//   DELETE /kv/:key      Raft 强一致删除 → 200 Chunked 或 307/503
//   GET    /admin/raft   节点状态 JSON（term / state / leaderId / commitIndex）
//   GET    /admin/scan   返回状态机全部 KV 条目（分页参数 offset / limit）
//   GET    /             静态文件 → staticDir/index.html（Web 仪表盘）
//   GET    /app.js       静态文件 → staticDir/app.js
//
// Leader 重定向策略（307 Temporary Redirect）：
//   非 Leader 收到写请求时，若已知 leaderId >= 0，
//   返回 307 + Location: http://127.0.0.1:{baseHttpPort + leaderId}/kv/:key
//   让客户端自动重试到正确节点；若 leaderId 未知（-1），返回 503。
//
// 异步写 + Chunked 流式响应：
//   Leader 收到写请求后，立即以 Transfer-Encoding: chunked 发送头部
//   并推送 {"status":"pending"} 首块（不阻塞 IO 线程）；
//   Raft apply 回调触发后，通过 queueInLoop 发送 {"status":"applied",...} 尾块
//   和终止块（0\r\n\r\n），全程无 future::wait 阻塞。
#include "Connection.h"
#include "http/HttpServer.h"
#include "kv/KvStateMachine.h"
#include "raft/RaftNode.h"
#include <cstdint>
#include <memory>
#include <string>

class KvHttpServer {
  public:
    // httpPort:     本节点监听的 HTTP 端口
    // baseHttpPort: 集群第 0 个节点的 HTTP 端口；节点 N 对应 baseHttpPort + N
    // staticDir:    静态文件目录（Web 仪表盘），默认 "examples/static/kv"
    KvHttpServer(raft::RaftNode &node, KvStateMachine &sm,
                 uint16_t httpPort, uint16_t baseHttpPort = 8901,
                 std::string staticDir = "examples/static/kv");
    ~KvHttpServer() = default;

    void start();
    void stop();

  private:
    // 从 URL path 中提取 /kv/<key> 后的 key 部分
    static std::string extractKey(const std::string &path);

    // 生成指向 leaderId 节点相同路径的 Location URL
    std::string leaderUrl(const std::string &path) const;

    // 异步写请求处理（PUT / DEL 共用）：
    //   非 Leader → 同步 307/503（resp 正常返回）
    //   Leader    → 立即发 chunked 头 + pending 块，setDeferred(true)，
    //               Raft apply 回调通过 queueInLoop 发结果块 + 终止块
    void handleWriteAsync(const std::string &cmd,
                          const std::string &path,
                          const HttpRequest &req,
                          HttpResponse *resp,
                          Connection *conn);

    raft::RaftNode   &node_;
    KvStateMachine   &sm_;
    uint16_t          baseHttpPort_;
    std::string       staticDir_;
    std::unique_ptr<HttpServer> srv_;
};
