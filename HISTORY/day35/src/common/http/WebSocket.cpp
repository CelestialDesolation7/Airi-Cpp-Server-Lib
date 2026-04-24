#include "http/WebSocket.h"
#include "Connection.h"
#include "log/Logger.h"
#include <algorithm>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════════
// 内置 SHA-1 实现 (RFC 3174)
// 仅用于 WebSocket 握手，非通用密码学用途。
// ═══════════════════════════════════════════════════════════════════════════════
namespace {

class SHA1 {
  public:
    SHA1() { reset(); }

    void update(const uint8_t *data, size_t len) {
        while (len > 0) {
            size_t space = 64 - bufLen_;
            size_t chunk = std::min(len, space);
            std::memcpy(buf_ + bufLen_, data, chunk);
            bufLen_ += chunk;
            data += chunk;
            len -= chunk;
            totalBits_ += chunk * 8;
            if (bufLen_ == 64) {
                processBlock(buf_);
                bufLen_ = 0;
            }
        }
    }

    void update(const std::string &s) {
        update(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    std::string digest() {
        // 保存原始消息长度（不含 padding）
        uint64_t bits = totalBits_;

        // 填充：先追加 0x80，再追加零，直到 bufLen_ ≡ 56 (mod 64)
        static const uint8_t padding[64] = {0x80};
        size_t padLen = (bufLen_ < 56) ? (56 - bufLen_) : (120 - bufLen_);
        update(padding, padLen);

        // 追加原始长度（大端 64-bit）
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = static_cast<uint8_t>((bits >> ((7 - i) * 8)) & 0xFF);
        update(lenBytes, 8);

        // 输出 20 字节摘要
        std::string result(20, '\0');
        for (int i = 0; i < 5; ++i) {
            result[i * 4 + 0] = static_cast<char>((h_[i] >> 24) & 0xFF);
            result[i * 4 + 1] = static_cast<char>((h_[i] >> 16) & 0xFF);
            result[i * 4 + 2] = static_cast<char>((h_[i] >> 8) & 0xFF);
            result[i * 4 + 3] = static_cast<char>((h_[i]) & 0xFF);
        }
        return result;
    }

  private:
    uint32_t h_[5]{};
    uint8_t buf_[64]{};
    size_t bufLen_{0};
    uint64_t totalBits_{0};

    void reset() {
        h_[0] = 0x67452301;
        h_[1] = 0xEFCDAB89;
        h_[2] = 0x98BADCFE;
        h_[3] = 0x10325476;
        h_[4] = 0xC3D2E1F0;
        bufLen_ = 0;
        totalBits_ = 0;
    }

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void processBlock(const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }
};

// Base64 编码表
const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// WebSocketConnection
// ═══════════════════════════════════════════════════════════════════════════════

WebSocketConnection::WebSocketConnection(Connection *conn) : conn_(conn) {}

void WebSocketConnection::sendText(const std::string &msg) { sendFrame(WsOpcode::kText, msg); }

void WebSocketConnection::sendBinary(const std::string &data) {
    sendFrame(WsOpcode::kBinary, data);
}

void WebSocketConnection::sendPing(const std::string &payload) {
    sendFrame(WsOpcode::kPing, payload);
}

void WebSocketConnection::sendPong(const std::string &payload) {
    sendFrame(WsOpcode::kPong, payload);
}

void WebSocketConnection::sendClose(uint16_t code, const std::string &reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason);
    sendFrame(WsOpcode::kClose, payload);
}

void WebSocketConnection::sendFrame(WsOpcode opcode, const std::string &payload) {
    // 服务端发出的帧不需要掩码
    std::string frame = ws::encodeFrame(opcode, payload, false, nullptr);
    conn_->send(std::move(frame));
}

// ═══════════════════════════════════════════════════════════════════════════════
// ws 命名空间：工具函数实现
// ═══════════════════════════════════════════════════════════════════════════════
namespace ws {

std::string sha1(const std::string &input) {
    SHA1 ctx;
    ctx.update(input);
    return ctx.digest();
}

std::string base64Encode(const std::string &input) {
    std::string result;
    const auto *data = reinterpret_cast<const uint8_t *>(input.data());
    size_t len = input.size();
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len)
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len)
            n |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        result.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? kBase64Chars[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? kBase64Chars[n & 0x3F] : '=');
    }
    return result;
}

std::string computeAcceptKey(const std::string &clientKey) {
    // RFC 6455 Section 4.2.2: GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    static const std::string kMagicGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return base64Encode(sha1(clientKey + kMagicGUID));
}

void applyMask(char *data, size_t len, const uint8_t maskKey[4]) {
    for (size_t i = 0; i < len; ++i)
        data[i] ^= static_cast<char>(maskKey[i % 4]);
}

std::string encodeFrame(WsOpcode opcode, const std::string &payload, bool mask,
                        const uint8_t maskKey[4]) {
    std::string frame;
    size_t payloadLen = payload.size();

    // 第 1 字节：FIN + opcode
    frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

    // 第 2 字节：MASK + payload length
    uint8_t maskBit = mask ? 0x80 : 0x00;
    if (payloadLen <= 125) {
        frame.push_back(static_cast<char>(maskBit | static_cast<uint8_t>(payloadLen)));
    } else if (payloadLen <= 0xFFFF) {
        frame.push_back(static_cast<char>(maskBit | 126));
        frame.push_back(static_cast<char>((payloadLen >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payloadLen & 0xFF));
    } else {
        frame.push_back(static_cast<char>(maskBit | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((payloadLen >> (i * 8)) & 0xFF));
    }

    // 掩码密钥
    if (mask && maskKey) {
        frame.append(reinterpret_cast<const char *>(maskKey), 4);
    }

    // 载荷
    if (mask && maskKey) {
        std::string masked = payload;
        applyMask(masked.data(), masked.size(), maskKey);
        frame.append(masked);
    } else {
        frame.append(payload);
    }

    return frame;
}

DecodeResult decodeFrame(const char *data, size_t len, WebSocketFrame &frame,
                         size_t &consumedBytes) {
    consumedBytes = 0;
    if (len < 2)
        return DecodeResult::kIncomplete;

    const auto *bytes = reinterpret_cast<const uint8_t *>(data);

    // 第 1 字节
    frame.fin = (bytes[0] & 0x80) != 0;
    uint8_t rsv = (bytes[0] >> 4) & 0x07;
    if (rsv != 0)
        return DecodeResult::kError; // 未协商扩展，RSV 必须为 0
    frame.opcode = static_cast<WsOpcode>(bytes[0] & 0x0F);

    // 第 2 字节
    frame.masked = (bytes[1] & 0x80) != 0;
    uint64_t payloadLen = bytes[1] & 0x7F;

    size_t headerLen = 2;
    if (payloadLen == 126) {
        if (len < 4)
            return DecodeResult::kIncomplete;
        payloadLen = (static_cast<uint64_t>(bytes[2]) << 8) | bytes[3];
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (len < 10)
            return DecodeResult::kIncomplete;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i)
            payloadLen = (payloadLen << 8) | bytes[2 + i];
        headerLen = 10;
        // 安全检查：最高位必须为 0
        if (payloadLen & (uint64_t(1) << 63))
            return DecodeResult::kError;
    }

    if (frame.masked)
        headerLen += 4;

    size_t totalLen = headerLen + static_cast<size_t>(payloadLen);
    if (len < totalLen)
        return DecodeResult::kIncomplete;

    // 读取掩码密钥
    if (frame.masked) {
        size_t maskOffset = headerLen - 4;
        std::memcpy(frame.maskKey, bytes + maskOffset, 4);
    }

    // 读取载荷
    frame.payload.assign(data + headerLen, static_cast<size_t>(payloadLen));

    // 解掩码
    if (frame.masked)
        applyMask(frame.payload.data(), frame.payload.size(), frame.maskKey);

    consumedBytes = totalLen;
    return DecodeResult::kComplete;
}

bool isUpgradeRequest(const std::string &upgradeHeader, const std::string &connectionHeader) {
    // 不区分大小写比较
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    return toLower(upgradeHeader) == "websocket" &&
           toLower(connectionHeader).find("upgrade") != std::string::npos;
}

std::string buildHandshakeResponse(const std::string &acceptKey) {
    std::string resp;
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: " + acceptKey + "\r\n";
    resp += "\r\n";
    return resp;
}

void handleWebSocketData(Connection *conn) {
    auto *ctx = conn->getContextAs<WebSocketConnectionContext>();
    if (!ctx || ctx->state == WebSocketConnectionContext::State::kClosed)
        return;

    Buffer *buf = conn->getInputBuffer();
    WebSocketConnection wsConn(conn);

    while (buf->readableBytes() > 0) {
        WebSocketFrame frame;
        size_t consumed = 0;
        auto result = decodeFrame(buf->peek(), buf->readableBytes(), frame, consumed);

        if (result == DecodeResult::kIncomplete)
            break;
        if (result == DecodeResult::kError) {
            LOG_WARN << "[WebSocket] 帧解析错误, fd=" << conn->getSocket()->getFd();
            wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError), "Protocol error");
            ctx->state = WebSocketConnectionContext::State::kClosing;
            return;
        }

        buf->retrieve(consumed);

        // 控制帧不能分片
        if (frame.isControl() && !frame.fin) {
            wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError),
                             "Fragmented control frame");
            ctx->state = WebSocketConnectionContext::State::kClosing;
            return;
        }

        // 处理各类帧
        switch (frame.opcode) {
        case WsOpcode::kPing:
            if (ctx->handler.onPing) {
                ctx->handler.onPing(wsConn, frame.payload);
            } else {
                wsConn.sendPong(frame.payload);
            }
            break;

        case WsOpcode::kPong:
            if (ctx->handler.onPong)
                ctx->handler.onPong(wsConn, frame.payload);
            break;

        case WsOpcode::kClose: {
            uint16_t code = static_cast<uint16_t>(WsCloseCode::kNoStatus);
            std::string reason;
            if (frame.payload.size() >= 2) {
                code = (static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[0])) << 8) |
                       static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[1]));
                reason = frame.payload.substr(2);
            }

            if (ctx->state == WebSocketConnectionContext::State::kClosing) {
                // 我们先发了 close，对方回了 close，完成关闭握手
                ctx->state = WebSocketConnectionContext::State::kClosed;
                if (ctx->handler.onClose)
                    ctx->handler.onClose(wsConn, code, reason);
                conn->close();
            } else {
                // 对方先发 close，我们回一个 close
                ctx->state = WebSocketConnectionContext::State::kClosing;
                wsConn.sendClose(code, reason);
                ctx->state = WebSocketConnectionContext::State::kClosed;
                if (ctx->handler.onClose)
                    ctx->handler.onClose(wsConn, code, reason);
                conn->close();
            }
            return;
        }

        case WsOpcode::kText:
        case WsOpcode::kBinary: {
            if (!ctx->fragmentBuffer.empty()) {
                // 收到新的数据帧但上一组分片还没结束
                wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError),
                                 "New message before fragment complete");
                ctx->state = WebSocketConnectionContext::State::kClosing;
                return;
            }
            if (frame.fin) {
                // 完整消息
                bool isBinary = (frame.opcode == WsOpcode::kBinary);
                if (ctx->handler.onMessage)
                    ctx->handler.onMessage(wsConn, frame.payload, isBinary);
            } else {
                // 分片开始
                ctx->fragmentOpcode = frame.opcode;
                ctx->fragmentBuffer = std::move(frame.payload);
            }
            break;
        }

        case WsOpcode::kContinuation: {
            if (ctx->fragmentBuffer.empty() && ctx->fragmentOpcode == WsOpcode::kContinuation) {
                wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError),
                                 "Unexpected continuation frame");
                ctx->state = WebSocketConnectionContext::State::kClosing;
                return;
            }
            ctx->fragmentBuffer.append(frame.payload);
            if (frame.fin) {
                bool isBinary = (ctx->fragmentOpcode == WsOpcode::kBinary);
                if (ctx->handler.onMessage)
                    ctx->handler.onMessage(wsConn, ctx->fragmentBuffer, isBinary);
                ctx->fragmentBuffer.clear();
                ctx->fragmentOpcode = WsOpcode::kContinuation;
            }
            break;
        }

        default:
            wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError), "Unknown opcode");
            ctx->state = WebSocketConnectionContext::State::kClosing;
            return;
        }
    }
}

} // namespace ws
