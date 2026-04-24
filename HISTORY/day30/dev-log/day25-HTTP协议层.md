# Day 25 — HTTP 协议层（HttpServer / HttpContext / HttpRequest / HttpResponse）

> **主题**：在 Reactor 网络库之上叠加 HTTP/1.x 协议栈——状态机解析、路由分发、响应序列化。  
> **基于**：Day 24（异步日志系统 AsyncLogging）

---

## 1. 引言

### 1.1 问题上下文

到 Day 24，Reactor 网络栈 + 定时器 + 异步日志全部就绪，但项目仍然只能跑 echo——没有任何应用层协议。HTTP/1.x 是 Web 世界的事实标准（也是 Day 26 文件服务、Day 28 性能测试、Day 36 muduo 横向对比的共同基线）。

HTTP/1.x 的解析有两个工程难点：(a) **TCP 粘包**——一次 read 可能拿到半个请求或多个请求拼在一起；(b) **Body 长度多样**——`Content-Length` 已知 / `chunked` 流式 / 无 body。muduo 用一个明确的有限状态机（FSM）逐字节驱动解析，每次只消费已读到的部分，剩余字节留给下次 read。

### 1.2 动机

HTTP 解析器是网络服务的"协议门面"。一个能正确处理粘包/半包的 FSM 解析器，可以直接复用到 HTTP keep-alive、HTTP/1.1 pipelining、甚至 WebSocket 升级握手。

把 HTTP 抽象为 `HttpServer`（继承组合 TcpServer）+ `HttpRequest` / `HttpResponse` / `HttpContext` 四件套，让上层业务只写 `[](req, resp){ ... }`，无需关心 TCP 字节流。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 字符串切分（split + scanf） | 教学 | 简单 | 无法处理粘包、错误恢复差 |
| 有限状态机逐字节驱动 (本日) | nginx / muduo / 本项目 | 内存零拷贝、粘包安全 | 状态多、初看复杂 |
| `http-parser` (C 库) | Node.js / nginx | 工业级、高度优化 | 引入 C 依赖 |
| `llhttp` | Node.js 现役 | C 移植自 JS、SIMD | 同上 |
| `boost::beast` | Boost | C++ 全栈 HTTP | 大型依赖 |
| `actix-web` / `axum` | Rust | 协程、宏 DSL | 跨语言 |

### 1.4 本日方案概述

本日实现：
1. `HttpRequest.h/cpp`：方法、URL、查询参数、请求头 map、body；method/version 字符串 ↔ 枚举。
2. `HttpResponse.h/cpp`：状态码、headers map、body；`serialize()` 拼成可发送字节流。
3. `HttpContext.h/cpp`：15 个状态的 FSM 解析器（`kExpectMethod` → `kExpectVersion` → `kExpectHeaders` → `kExpectBody` → `kGotAll` 等），逐字符驱动；通过 `consumed` 出参告知调用方"我消费了多少字节"，剩余留在 Buffer 等下次 read。
4. `HttpServer.h/cpp`：内部组合 TcpServer，`onMessage` 中喂字节给 HttpContext，解析完一条调用 `httpCallback_(req, resp)`。
5. `Connection` 新增 `std::any context_` 通用上下文槽（让 HTTP 层把 HttpContext 挂到 Connection 上）。
6. `http_server.cpp` 示例：4 个路由（/、/hello、/echo、/query）。

下一天在此基础上做完整文件服务、登录表单、空闲超时关闭。

---
## 2. 文件变更总览

| 文件 | 状态 | 说明 |
|------|------|------|
| `include/http/HttpRequest.h` | **新增** | HTTP 请求报文数据类：方法、URL、查询参数、请求头、Body |
| `common/http/HttpRequest.cpp` | **新增** | HttpRequest 方法/版本字符串转换、reset |
| `include/http/HttpResponse.h` | **新增** | HTTP 响应报文构建器：状态码、头部、Body |
| `common/http/HttpResponse.cpp` | **新增** | HttpResponse::serialize() 序列化为可发送字节流 |
| `include/http/HttpContext.h` | **新增** | HTTP/1.x 请求有限状态机解析器（15 个状态） |
| `common/http/HttpContext.cpp` | **新增** | 状态机核心：逐字符驱动状态迁移，支持 TCP 粘包 |
| `include/http/HttpServer.h` | **新增** | HttpServer：在 TcpServer 上封装 HTTP 协议处理 |
| `common/http/HttpServer.cpp` | **新增** | HttpServer：连接管理、消息分派、请求路由 |
| `include/Connection.h` | **修改** | 新增 `std::any context_` 通用上下文槽 + `setContext/getContextAs` |
| `http_server.cpp` | **新增** | HTTP 示例服务器：/ /hello /echo /query 四个路由 |

---

## 3. 模块全景与所有权树

```
http_server main()
  │ HttpServer srv
  │ srv.setHttpCallback(handleRequest)
  │ srv.start()
  ▼
HttpServer
├── TcpServer（组合）
│   ├── mainReactor: Eventloop
│   │   ├── Poller, Channel, TimerQueue
│   │   └── Acceptor → accept → 分配 Connection
│   ├── EventLoopThreadPool
│   │   └── subReactor × N
│   └── connections: map<int, unique_ptr<Connection>>
│       └── Connection
│           ├── Socket, Channel, Buffer(in/out)
│           └── context_: std::any ← HttpContext ★ NEW
│               ├── state_: State (FSM)
│               └── request_: HttpRequest
│                   ├── method_, version_, url_
│                   ├── queryParams_: map
│                   ├── headers_: map
│                   └── body_: string
├── httpCallback_: function → handleRequest()
│   └── 填写 HttpResponse
│       ├── statusCode_, statusMessage_
│       ├── headers_: map
│       ├── body_: string
│       └── serialize() → "HTTP/1.1 200 OK\r\n..."
└── Logger (LOG_INFO/LOG_WARN)
```

---

## 4. 全流程调用链

### 4.1 HTTP 请求处理全流程

```
客户端发送：
  GET /hello HTTP/1.1\r\n
  Host: 127.0.0.1:8888\r\n
  Connection: keep-alive\r\n
  \r\n

  ① TCP 数据到达
  ──────────────────────────────────────────────────
  Channel::handleEvent() → Connection::Read()
    └── Buffer::readFd() → inputBuffer_ 积累原始字节

  ② 消息回调
  ──────────────────────────────────────────────────
  Connection::Business() → onMessageCallback_
    └── HttpServer::onMessage(conn)
        ├── ctx = conn->getContextAs<HttpContext>()
        ├── ctx->parse(buf->peek(), buf->readableBytes())
        │   └── 状态机逐字符解析（见 §4.2）
        ├── buf->retrieve(...)  消耗已解析字节
        └── if ctx->isComplete():
            HttpServer::onRequest(conn, ctx->request())

  ③ 路由分发
  ──────────────────────────────────────────────────
  HttpServer::onRequest(conn, req)
    ├── 判断 Connection: close / keep-alive
    ├── HttpResponse resp(close)
    ├── httpCallback_(req, &resp)   ← 用户注册的 handleRequest
    │   └── resp.setStatus(200, "OK")
    │       resp.setContentType("text/plain")
    │       resp.setBody("Hello, World!\n")
    ├── conn->send(resp.serialize())
    │   └── "HTTP/1.1 200 OK\r\n
    │        Content-Length: 14\r\n
    │        Connection: keep-alive\r\n
    │        Content-Type: text/plain\r\n
    │        \r\n
    │        Hello, World!\n"
    └── if close: conn->close()

  ④ Keep-Alive 复用
  ──────────────────────────────────────────────────
  ctx->reset()  → 状态机回到 kStart，准备解析下一个请求
```

---

## 5. 代码逐段解析

### 5.1 HttpRequest — 请求数据类

```cpp
class HttpRequest {
  public:
    enum class Method { kInvalid, kGet, kPost, kHead, kPut, kDelete };
    enum class Version { kUnknown, kHttp10, kHttp11 };

    bool setMethod(const std::string &m);    // "GET" → kGet
    void setVersion(const std::string &v);   // "1.1" → kHttp11
    void addQueryParam(key, value);          // ?key=value
    void addHeader(key, value);              // Header: value
    void reset();                            // keep-alive 复用
  private:
    Method method_; Version version_;
    std::string url_;
    std::map<std::string, std::string> queryParams_, headers_;
    std::string body_;
};
```

纯数据对象，所有字段由 HttpContext 状态机填充。

### 5.2 HttpContext — 有限状态机解析器

```
状态迁移图：

kStart ──[A-Z]──→ kMethod ──[空格]──→ kBeforeUrl ──[/]──→ kUrl
                                                            │
                                                     [?]    [空格]
                                                      ▼      ▼
                                               kQueryKey  kBeforeProtocol
                                                  │          │
                                               [=]        [A-Z]
                                                  ▼          ▼
                                            kQueryValue  kProtocol ──[/]──→ kBeforeVersion
                                                  │                            │
                                               [&]  [空格]                  [数字]
                                                ▼     ▼                       ▼
                                          kQueryKey  kBeforeProtocol    kVersion ──[\r]──→ kHeaderKey
                                                                                           │
                                                                                    [:]    [\r\n空行]
                                                                                     ▼        ▼
                                                                              kHeaderValue  kBody/kComplete
                                                                                   │
                                                                                [\r]
                                                                                   ▼
                                                                              kHeaderKey（循环）
```

```cpp
bool HttpContext::parse(const char *data, int len) {
    const char *p = data;
    while (p < end && state_ != kComplete && state_ != kInvalid) {
        char ch = *p;
        switch (state_) {
        case State::kMethod:
            if (isupper(ch)) tokenBuf_ += ch;
            else if (isblank(ch)) {
                request_.setMethod(tokenBuf_);  // "GET" → kGet
                state_ = kBeforeUrl;
            }
            break;
        // ... 15 个状态
        }
        ++p;
    }
    return state_ != kInvalid;
}
```

**TCP 粘包/分包处理**：

- 状态 + tokenBuf_ 在调用间持续保存。
- 一次 parse() 可能只喂入半行数据，下次继续解析。

**Body 处理**：

- 根据 `Content-Length` 读取指定字节数，支持跨 TCP 段分片。

### 5.3 HttpResponse — 响应构建与序列化

```cpp
std::string HttpResponse::serialize() const {
    std::string result;
    result.reserve(256 + body_.size());

    // 状态行：HTTP/1.1 200 OK\r\n
    result += "HTTP/1.1 " + to_string(statusCode_) + " " + statusMessage_ + "\r\n";

    // 固定头部
    result += "Content-Length: " + to_string(body_.size()) + "\r\n";
    result += closeConnection_ ? "Connection: close\r\n" : "Connection: keep-alive\r\n";

    // 用户自定义头部
    for (auto &[k, v] : headers_)
        result += k + ": " + v + "\r\n";

    result += "\r\n" + body_;
    return result;
}
```

**始终包含 Content-Length**：客户端无需等待 TCP 关闭即可确定响应体结束。

### 5.4 HttpServer — TCP 层到 HTTP 层的桥接

```cpp
HttpServer::HttpServer() : server_(make_unique<TcpServer>()) {
    server_->newConnect(bind(&HttpServer::onNewConnection, this, _1));
    server_->onMessage(bind(&HttpServer::onMessage, this, _1));
}
```

**Connection 通用上下文**：

```cpp
// Connection.h 新增
std::any context_;
void setContext(std::any ctx) { context_ = std::move(ctx); }
template <typename T>
T *getContextAs() { return std::any_cast<T>(&context_); }
```

`std::any` 让 TCP 层对 HTTP 协议一无所知——HttpContext 通过类型擦除存储在每个连接中。

### 5.5 http_server.cpp — 示例应用

```cpp
static void handleRequest(const HttpRequest &req, HttpResponse *resp) {
    if (req.method() == Method::kGet && url == "/")      → HTML 首页
    if (req.method() == Method::kGet && url == "/hello")  → "Hello, World!"
    if (req.method() == Method::kPost && url == "/echo")  → 返回请求体
    if (req.method() == Method::kGet && url == "/query")  → 打印查询参数
    else                                                   → 404
}
```

四个路由演示 GET/POST + URL 参数 + Body echo。

---

### 5.6 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day25/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day25/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 类 | 单一职责 |
|----|----------|
| `HttpRequest` | HTTP 请求报文的数据表示（只读，由 Context 填充） |
| `HttpResponse` | HTTP 响应报文的构建与序列化 |
| `HttpContext` | 有限状态机解析 HTTP/1.x 请求报文 |
| `HttpServer` | 在 TcpServer 上封装 HTTP 协议，桥接 TCP→HTTP |
| `Connection::context_` | 通用上下文槽，实现 TCP 层与协议层解耦 |

---

## 7. 协议层与网络层的分层设计

```
┌─────────────────────────────────────────────┐
│  应用层  │  http_server.cpp  handleRequest  │
├──────────┼──────────────────────────────────┤
│  协议层  │  HttpServer / HttpContext         │  Day 25
│          │  HttpRequest / HttpResponse        │
├──────────┼──────────────────────────────────┤
│  传输层  │  TcpServer / Connection / Buffer  │  Day 19-22
│          │  Acceptor / EventLoopThreadPool    │
├──────────┼──────────────────────────────────┤
│  事件层  │  EventLoop / Channel / Poller     │  Day 06-18
│          │  TimerQueue                        │  Day 23
├──────────┼──────────────────────────────────┤
│  基础层  │  Socket / InetAddress / Logger    │  Day 01-05, 24
└──────────┴──────────────────────────────────┘
```

每层只依赖下层接口，不越级调用。

---

## 8. 局限与后续

| 当前局限 | 后续改进方向 |
|----------|------------|
| 仅支持 GET/POST，无 chunked 传输 | 添加 Transfer-Encoding: chunked 支持 |
| 无静态文件服务 | 添加文件读取 + MIME 类型映射 |
| URL 未做解码（%20 等） | 添加 URL decode |
| 路由用 if-else 硬编码 | 添加路由表（前缀树或 map） |
| Connection 生命周期为裸指针 | 改为 shared_ptr + weak_ptr 安全管理 |
| 无 HTTPS/TLS 支持 | 集成 OpenSSL/BoringSSL |
| **→ Day 26**：HTTP 应用层演示（静态文件服务、表单处理等） | |
