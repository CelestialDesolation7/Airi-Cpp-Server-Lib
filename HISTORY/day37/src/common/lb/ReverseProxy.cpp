#include "lb/ReverseProxy.h"
#include "lb/Backend.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>
#include <string>

// ── 上游 HTTP/1.0 阻塞客户端 ──────────────────────────────────────────────────
// 使用 HTTP/1.0：服务端发完响应即关闭连接，读取到 EOF = 读完整响应，无需解析 chunk。
// SO_RCVTIMEO / SO_SNDTIMEO = 500 ms：避免慢后端无限阻塞 I/O 线程。

ReverseProxy::UpstreamResult ReverseProxy::callUpstream(const std::string &host, int port,
                                                         const HttpRequest &req) {
    UpstreamResult result;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return result;

    // 500 ms 读写超时
    struct timeval tv{0, 500000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

#ifdef SO_NOSIGPIPE
    // macOS：避免往关闭的连接写时产生 SIGPIPE，用 socket 选项代替 MSG_NOSIGNAL
    int nosigpipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

    // Connect
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return result;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return result;
    }

    // ── 构造 HTTP/1.0 请求 ──────────────────────────────────────────────────
    // 跳过 hop-by-hop 头（这些头只对单段连接有意义，不应转发给上游）
    static const std::vector<std::string> kHopByHop = {
        "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
        "te", "trailers", "transfer-encoding", "upgrade", "host",
    };
    auto isHopByHop = [&](const std::string &k) {
        return std::find(kHopByHop.begin(), kHopByHop.end(), k) != kHopByHop.end();
    };

    std::string raw;
    raw.reserve(512 + req.body().size());
    raw += req.methodString() + " " + req.url() + " HTTP/1.0\r\n";
    raw += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    raw += "X-Forwarded-For: proxy\r\n";

    for (const auto &[k, v] : req.headers()) {
        if (!isHopByHop(k)) raw += k + ": " + v + "\r\n";
    }

    if (!req.body().empty())
        raw += "Content-Length: " + std::to_string(req.body().size()) + "\r\n";

    raw += "\r\n";
    raw += req.body();

    // ── 发送 ────────────────────────────────────────────────────────────────
    const char *ptr = raw.data();
    size_t      rem = raw.size();
    while (rem > 0) {
        ssize_t n = ::send(fd, ptr, rem, 0);
        if (n <= 0) { ::close(fd); return result; }
        ptr += n;
        rem -= static_cast<size_t>(n);
    }

    // ── 接收直到 EOF（HTTP/1.0 无 Content-Length 也能正确终止）────────────
    std::string response;
    response.reserve(4096);
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        response.append(buf, static_cast<size_t>(n));
    ::close(fd);

    if (response.empty()) return result;

    // ── 解析响应头 ──────────────────────────────────────────────────────────
    size_t sep = response.find("\r\n\r\n");
    if (sep == std::string::npos) return result;

    std::string header_section = response.substr(0, sep);
    result.body                = response.substr(sep + 4);

    // 状态行：HTTP/1.x NNN Message
    size_t eol  = header_section.find("\r\n");
    std::string sl = (eol != std::string::npos) ? header_section.substr(0, eol) : header_section;
    if (sl.size() >= 12) {
        try {
            result.statusCode = std::stoi(sl.substr(9, 3));
            result.statusMsg  = sl.size() > 13 ? sl.substr(13) : "OK";
        } catch (...) {
            result.statusCode = 200;
            result.statusMsg  = "OK";
        }
    } else {
        result.statusCode = 200;
        result.statusMsg  = "OK";
    }

    // 扫描响应头，提取 Content-Type（覆盖默认值）
    std::istringstream iss(header_section);
    std::string        line;
    std::getline(iss, line); // skip status line
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(": ");
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "content-type")
            result.contentType = line.substr(colon + 2);
    }

    result.ok = true;
    return result;
}

// ── 主处理逻辑 ────────────────────────────────────────────────────────────────
void ReverseProxy::handle(const HttpRequest &req, HttpResponse *resp) {
    Backend *b = pool_.pick(req);
    if (!b) {
        resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable,
                        "Service Unavailable");
        resp->setContentType("text/plain");
        resp->setBody("No backends available\n");
        return;
    }

    b->inflight.fetch_add(1, std::memory_order_relaxed);
    UpstreamResult res = callUpstream(b->host, b->port, req);
    b->inflight.fetch_sub(1, std::memory_order_relaxed);

    if (!res.ok) {
        b->onFailure();
        resp->setStatus(HttpResponse::StatusCode::k502BadGateway, "Bad Gateway");
        resp->setContentType("text/plain");
        resp->setBody("Upstream connection failed\n");
        return;
    }

    b->onSuccess();
    resp->setStatus(static_cast<HttpResponse::StatusCode>(res.statusCode), res.statusMsg);
    resp->setContentType(res.contentType);
    resp->setBody(res.body);
}
