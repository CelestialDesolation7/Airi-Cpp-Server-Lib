#pragma once
#include "Connection.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════════
// WebSocket 协议支持 (RFC 6455)
//
// 帧格式 (简化)：
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-------+-+-------------+-------------------------------+
//  |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
//  |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
//  |N|V|V|V|       |S|             |   (if payload len==126/127)   |
//  | |1|2|3|       |K|             |                               |
//  +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
//  |     Extended payload length continued, if payload len == 127  |
//  + - - - - - - - - - - - - - - - +-------------------------------+
//  |                               |Masking-key, if MASK set to 1  |
//  +-------------------------------+-------------------------------+
//  | Masking-key (continued)       |          Payload Data         |
//  +-------------------------------- - - - - - - - - - - - - - - - +
//  :                     Payload Data continued ...                :
//  + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
//  |                     Payload Data (continued)                  |
//  +---------------------------------------------------------------+
// ═══════════════════════════════════════════════════════════════════════════════

// ── WebSocket 帧操作码 ─────────────────────────────────────────────────────────
enum class WsOpcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    // 0x3-0x7 保留
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
    // 0xB-0xF 保留
};

// ── 解析后的 WebSocket 帧 ──────────────────────────────────────────────────────
struct WebSocketFrame {
    bool fin{true};                   // FIN 位：1 表示最后一个分片
    WsOpcode opcode{WsOpcode::kText}; // 操作码
    bool masked{false};               // MASK 位
    uint8_t maskKey[4]{0, 0, 0, 0};   // 4 字节掩码密钥
    std::string payload;              // 载荷数据（已解掩码）

    bool isControl() const { return static_cast<uint8_t>(opcode) >= 0x8; }
};

// ── WebSocket 关闭状态码 ───────────────────────────────────────────────────────
enum class WsCloseCode : uint16_t {
    kNormal = 1000,
    kGoingAway = 1001,
    kProtocolError = 1002,
    kUnsupported = 1003,
    kNoStatus = 1005,
    kAbnormal = 1006,
    kInvalidPayload = 1007,
    kPolicyViolation = 1008,
    kMessageTooBig = 1009,
};

// ── WebSocket 事件回调 ─────────────────────────────────────────────────────────
class WebSocketConnection; // 前向声明
struct WebSocketHandler {
    std::function<void(WebSocketConnection &)> onOpen;
    std::function<void(WebSocketConnection &, const std::string &msg, bool isBinary)> onMessage;
    std::function<void(WebSocketConnection &, uint16_t code, const std::string &reason)> onClose;
    std::function<void(WebSocketConnection &, const std::string &payload)> onPing;
    std::function<void(WebSocketConnection &, const std::string &payload)> onPong;
};

// ── WebSocket 连接状态 ─────────────────────────────────────────────────────────
// 存放于 Connection::context_ 中（std::any），替换原有的 HttpConnectionContext
struct WebSocketConnectionContext {
    enum class State { kOpen, kClosing, kClosed };

    WebSocketHandler handler;
    State state{State::kOpen};

    // 分片消息累积
    std::string fragmentBuffer;
    WsOpcode fragmentOpcode{WsOpcode::kContinuation};
};

// ── WebSocket 连接封装 ─────────────────────────────────────────────────────────
// 对外提供发送文本/二进制/ping/pong/close 等接口
class WebSocketConnection {
  public:
    explicit WebSocketConnection(Connection *conn);

    // 发送消息
    void sendText(const std::string &msg);
    void sendBinary(const std::string &data);
    void sendPing(const std::string &payload = "");
    void sendPong(const std::string &payload = "");
    void sendClose(uint16_t code = static_cast<uint16_t>(WsCloseCode::kNormal),
                   const std::string &reason = "");

    Connection *rawConnection() { return conn_; }

  private:
    void sendFrame(WsOpcode opcode, const std::string &payload);
    Connection *conn_;
};

// ── WebSocket 工具函数 ─────────────────────────────────────────────────────────
namespace ws {

// 帧编码：服务端发出的帧不需要掩码
std::string encodeFrame(WsOpcode opcode, const std::string &payload, bool mask = false,
                        const uint8_t maskKey[4] = nullptr);

// 帧解码结果
enum class DecodeResult {
    kComplete,   // 成功解码一帧
    kIncomplete, // 数据不足，等待更多
    kError,      // 协议错误
};

// 帧解码：从缓冲区中解析一帧，consumedBytes 输出消费的字节数
DecodeResult decodeFrame(const char *data, size_t len, WebSocketFrame &frame,
                         size_t &consumedBytes);

// 掩码/解掩码（对称操作）
void applyMask(char *data, size_t len, const uint8_t maskKey[4]);

// SHA-1 哈希（返回 20 字节原始摘要）
std::string sha1(const std::string &input);

// Base64 编码
std::string base64Encode(const std::string &input);

// 计算 Sec-WebSocket-Accept 值
// accept = base64( sha1( clientKey + "258EAFA5-E914-47DA-95CA-5AB5DC11B451" ) )
std::string computeAcceptKey(const std::string &clientKey);

// 检查 HTTP 请求是否为 WebSocket 升级请求
bool isUpgradeRequest(const std::string &upgradeHeader, const std::string &connectionHeader);

// 生成 101 Switching Protocols 握手响应
std::string buildHandshakeResponse(const std::string &acceptKey);

// 处理 WebSocket 连接上收到的数据
void handleWebSocketData(Connection *conn);

} // namespace ws
