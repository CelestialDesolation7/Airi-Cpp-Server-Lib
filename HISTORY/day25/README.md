# Day 25 — HTTP 协议层

在 Reactor 网络库之上叠加 HTTP/1.x 协议栈：有限状态机解析请求（HttpContext）、路由分发（HttpServer）、响应序列化（HttpResponse）。

## 新增模块

| 文件 | 说明 |
|------|------|
| `include/http/HttpRequest.h` | HTTP 请求数据类 |
| `common/http/HttpRequest.cpp` | 方法/版本转换、reset |
| `include/http/HttpResponse.h` | HTTP 响应构建器 |
| `common/http/HttpResponse.cpp` | serialize() 序列化 |
| `include/http/HttpContext.h` | 请求有限状态机（15 状态） |
| `common/http/HttpContext.cpp` | 逐字符状态迁移，支持 TCP 粘包 |
| `include/http/HttpServer.h` | TcpServer 上的 HTTP 封装 |
| `common/http/HttpServer.cpp` | 连接管理、消息分派、路由 |
| `http_server.cpp` | 示例 HTTP 服务器（4 路由） |

## 构建 & 运行

```bash
cd HISTORY/day25
cmake -S . -B build && cmake --build build -j4

# 运行 HTTP 服务器
./build/http_server
# 浏览器访问 http://127.0.0.1:8888/

# 或用 curl 测试
curl http://127.0.0.1:8888/hello
curl -X POST -d "echo this" http://127.0.0.1:8888/echo
curl "http://127.0.0.1:8888/query?name=world&lang=cpp"
```

## 可执行文件

| 名称 | 说明 |
|------|------|
| `http_server` | HTTP 示例服务器（/、/hello、/echo、/query） |
| `server` | Echo TCP 服务器 |
| `client` | TCP 客户端 |
| `LogTest` | 日志系统测试 |
| `TimerTest` | 定时器测试 |
| `ThreadPoolTest` | 线程池测试 |
| `StressTest` | 压力测试客户端 |

## HTTP 路由

| 方法 | 路径 | 响应 |
|------|------|------|
| GET | `/` | HTML 首页 |
| GET | `/hello` | `Hello, World!` |
| POST | `/echo` | 原样返回请求体 |
| GET | `/query?k=v` | 打印 URL 查询参数 |
| 其他 | 任意 | 404 Not Found |

## 核心设计

- **状态机解析**：15 个状态逐字符处理，天然支持 TCP 粘包/分包
- **Connection::context_**：`std::any` 类型擦除，TCP 层与协议层完全解耦
- **Keep-Alive**：解析完一个请求后 `ctx->reset()` 复用连接
- **分层架构**：应用层 → 协议层 → 传输层 → 事件层 → 基础层
