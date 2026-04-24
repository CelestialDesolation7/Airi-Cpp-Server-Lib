#include "http/WebSocket.h"

#include <cstring>
#include <gtest/gtest.h>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════════
// SHA-1 + Base64 + 握手密钥
// ═══════════════════════════════════════════════════════════════════════════════

// RFC 6455 Section 4.2.2 给出的测试向量：
//   客户端发送 Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
//   服务端应返回 Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
TEST(WebSocketTest, HandshakeAcceptKey_RFC6455) {
    const std::string clientKey = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    EXPECT_EQ(ws::computeAcceptKey(clientKey), expected);
}

TEST(WebSocketTest, SHA1_Empty) {
    // SHA-1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709
    std::string hash = ws::sha1("");
    ASSERT_EQ(hash.size(), 20u);
    // 验证前 4 字节
    EXPECT_EQ(static_cast<uint8_t>(hash[0]), 0xda);
    EXPECT_EQ(static_cast<uint8_t>(hash[1]), 0x39);
    EXPECT_EQ(static_cast<uint8_t>(hash[2]), 0xa3);
    EXPECT_EQ(static_cast<uint8_t>(hash[3]), 0xee);
}

TEST(WebSocketTest, SHA1_ABC) {
    // SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
    std::string hash = ws::sha1("abc");
    ASSERT_EQ(hash.size(), 20u);
    EXPECT_EQ(static_cast<uint8_t>(hash[0]), 0xa9);
    EXPECT_EQ(static_cast<uint8_t>(hash[1]), 0x99);
    EXPECT_EQ(static_cast<uint8_t>(hash[2]), 0x3e);
    EXPECT_EQ(static_cast<uint8_t>(hash[3]), 0x36);
}

TEST(WebSocketTest, Base64_Empty) { EXPECT_EQ(ws::base64Encode(""), ""); }

TEST(WebSocketTest, Base64_Basic) {
    EXPECT_EQ(ws::base64Encode("f"), "Zg==");
    EXPECT_EQ(ws::base64Encode("fo"), "Zm8=");
    EXPECT_EQ(ws::base64Encode("foo"), "Zm9v");
    EXPECT_EQ(ws::base64Encode("foob"), "Zm9vYg==");
    EXPECT_EQ(ws::base64Encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(ws::base64Encode("foobar"), "Zm9vYmFy");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 掩码 / 解掩码
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, MaskUnmask_Roundtrip) {
    const std::string original = "Hello, WebSocket!";
    const uint8_t maskKey[4] = {0x37, 0xfa, 0x21, 0x3d};

    std::string masked = original;
    ws::applyMask(masked.data(), masked.size(), maskKey);
    EXPECT_NE(masked, original); // 掩码后应与原始不同

    ws::applyMask(masked.data(), masked.size(), maskKey);
    EXPECT_EQ(masked, original); // 再次掩码=解掩码
}

TEST(WebSocketTest, MaskUnmask_EmptyPayload) {
    const uint8_t maskKey[4] = {0x12, 0x34, 0x56, 0x78};
    std::string empty;
    ws::applyMask(empty.data(), empty.size(), maskKey);
    EXPECT_EQ(empty, "");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 帧编码 / 解码
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, EncodeDecodeText_Unmasked) {
    const std::string payload = "Hello";
    std::string encoded = ws::encodeFrame(WsOpcode::kText, payload);

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);

    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_EQ(consumed, encoded.size());
    EXPECT_TRUE(frame.fin);
    EXPECT_EQ(frame.opcode, WsOpcode::kText);
    EXPECT_FALSE(frame.masked);
    EXPECT_EQ(frame.payload, "Hello");
}

TEST(WebSocketTest, EncodeDecodeBinary_Masked) {
    const std::string payload = "Binary data here!";
    const uint8_t maskKey[4] = {0xAB, 0xCD, 0xEF, 0x01};

    std::string encoded = ws::encodeFrame(WsOpcode::kBinary, payload, true, maskKey);

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);

    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_TRUE(frame.fin);
    EXPECT_EQ(frame.opcode, WsOpcode::kBinary);
    EXPECT_TRUE(frame.masked);
    EXPECT_EQ(frame.payload, payload); // 解码后应恢复原始
}

TEST(WebSocketTest, EncodeDecode_MediumPayload) {
    // 126 <= len <= 65535 → 16-bit extended length
    std::string payload(200, 'X');
    std::string encoded = ws::encodeFrame(WsOpcode::kText, payload);

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);

    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_EQ(frame.payload, payload);
}

TEST(WebSocketTest, EncodeDecode_LargePayload) {
    // len > 65535 → 64-bit extended length
    std::string payload(70000, 'Y');
    std::string encoded = ws::encodeFrame(WsOpcode::kBinary, payload);

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);

    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_EQ(frame.payload.size(), 70000u);
    EXPECT_EQ(frame.payload, payload);
}

TEST(WebSocketTest, Decode_Incomplete) {
    // 只给 1 字节，应返回 Incomplete
    const char byte = static_cast<char>(0x81); // FIN + Text
    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(&byte, 1, frame, consumed);
    EXPECT_EQ(result, ws::DecodeResult::kIncomplete);
}

TEST(WebSocketTest, Decode_IncompletePayload) {
    // 帧头说 payload 长度 5，但只给了 2 字节的头
    std::string encoded = ws::encodeFrame(WsOpcode::kText, "Hello");
    // 截断到只有头部
    std::string truncated = encoded.substr(0, 3);

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(truncated.data(), truncated.size(), frame, consumed);
    EXPECT_EQ(result, ws::DecodeResult::kIncomplete);
}

TEST(WebSocketTest, Decode_RSVBitsError) {
    // RSV 位非零应报错
    char data[7] = {};
    data[0] = static_cast<char>(0x80 | 0x10 | 0x01); // FIN + RSV1 + Text
    data[1] = 0x00;                                  // 长度 0

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(data, 2, frame, consumed);
    EXPECT_EQ(result, ws::DecodeResult::kError);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Close 帧编码
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, CloseFrame_Encode) {
    // 构造一个带状态码的 close 帧
    std::string closePayload;
    uint16_t code = 1000;
    closePayload.push_back(static_cast<char>((code >> 8) & 0xFF));
    closePayload.push_back(static_cast<char>(code & 0xFF));
    closePayload.append("Normal");

    std::string encoded = ws::encodeFrame(WsOpcode::kClose, closePayload);

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);

    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_EQ(frame.opcode, WsOpcode::kClose);
    EXPECT_GE(frame.payload.size(), 2u);

    uint16_t parsedCode = (static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[0])) << 8) |
                          static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[1]));
    EXPECT_EQ(parsedCode, 1000u);
    EXPECT_EQ(frame.payload.substr(2), "Normal");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Ping / Pong 帧
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, PingPong_Roundtrip) {
    const std::string pingPayload = "keepalive";

    std::string encoded = ws::encodeFrame(WsOpcode::kPing, pingPayload);
    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);

    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_EQ(frame.opcode, WsOpcode::kPing);
    EXPECT_EQ(frame.payload, pingPayload);

    // Pong 也一样
    encoded = ws::encodeFrame(WsOpcode::kPong, pingPayload);
    result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);

    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_EQ(frame.opcode, WsOpcode::kPong);
    EXPECT_EQ(frame.payload, pingPayload);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 分片消息重组（模拟帧序列）
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, Fragmentation_ManualReassembly) {
    // 模拟分片：[Text, FIN=0, "Hel"] + [Continuation, FIN=0, "lo "] + [Continuation, FIN=1,
    // "World"] 手动构建分片帧字节流

    // 帧 1: opcode=Text, FIN=0, payload="Hel"
    std::string frame1;
    frame1.push_back(static_cast<char>(0x01)); // FIN=0, opcode=Text
    frame1.push_back(static_cast<char>(3));    // len=3
    frame1.append("Hel");

    // 帧 2: opcode=Continuation, FIN=0, payload="lo "
    std::string frame2;
    frame2.push_back(static_cast<char>(0x00)); // FIN=0, opcode=Continuation
    frame2.push_back(static_cast<char>(3));    // len=3
    frame2.append("lo ");

    // 帧 3: opcode=Continuation, FIN=1, payload="World"
    std::string frame3;
    frame3.push_back(static_cast<char>(0x80)); // FIN=1, opcode=Continuation
    frame3.push_back(static_cast<char>(5));    // len=5
    frame3.append("World");

    // 解码帧 1
    WebSocketFrame f;
    size_t consumed = 0;
    auto r = ws::decodeFrame(frame1.data(), frame1.size(), f, consumed);
    EXPECT_EQ(r, ws::DecodeResult::kComplete);
    EXPECT_FALSE(f.fin);
    EXPECT_EQ(f.opcode, WsOpcode::kText);
    EXPECT_EQ(f.payload, "Hel");

    // 解码帧 2
    r = ws::decodeFrame(frame2.data(), frame2.size(), f, consumed);
    EXPECT_EQ(r, ws::DecodeResult::kComplete);
    EXPECT_FALSE(f.fin);
    EXPECT_EQ(f.opcode, WsOpcode::kContinuation);
    EXPECT_EQ(f.payload, "lo ");

    // 解码帧 3
    r = ws::decodeFrame(frame3.data(), frame3.size(), f, consumed);
    EXPECT_EQ(r, ws::DecodeResult::kComplete);
    EXPECT_TRUE(f.fin);
    EXPECT_EQ(f.opcode, WsOpcode::kContinuation);
    EXPECT_EQ(f.payload, "World");

    // 重组
    std::string reassembled = "Hel" + std::string("lo ") + "World";
    EXPECT_EQ(reassembled, "Hello World");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 多帧连续解析
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, MultipleFrames_InOneBuffer) {
    std::string buf;
    buf += ws::encodeFrame(WsOpcode::kText, "AAA");
    buf += ws::encodeFrame(WsOpcode::kText, "BBB");
    buf += ws::encodeFrame(WsOpcode::kPing, "");

    const char *ptr = buf.data();
    size_t remaining = buf.size();
    int count = 0;

    while (remaining > 0) {
        WebSocketFrame frame;
        size_t consumed = 0;
        auto result = ws::decodeFrame(ptr, remaining, frame, consumed);
        if (result != ws::DecodeResult::kComplete)
            break;
        ptr += consumed;
        remaining -= consumed;
        ++count;
    }

    EXPECT_EQ(count, 3);
    EXPECT_EQ(remaining, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 升级检测
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, IsUpgradeRequest) {
    EXPECT_TRUE(ws::isUpgradeRequest("websocket", "Upgrade"));
    EXPECT_TRUE(ws::isUpgradeRequest("WebSocket", "keep-alive, Upgrade"));
    EXPECT_TRUE(ws::isUpgradeRequest("WEBSOCKET", "upgrade"));
    EXPECT_FALSE(ws::isUpgradeRequest("", "Upgrade"));
    EXPECT_FALSE(ws::isUpgradeRequest("websocket", ""));
    EXPECT_FALSE(ws::isUpgradeRequest("http2", "Upgrade"));
}

TEST(WebSocketTest, HandshakeResponse_Format) {
    std::string resp = ws::buildHandshakeResponse("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_NE(resp.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(resp.find("Upgrade: websocket"), std::string::npos);
    EXPECT_NE(resp.find("Connection: Upgrade"), std::string::npos);
    EXPECT_NE(resp.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
    // 以 \r\n\r\n 结尾
    EXPECT_EQ(resp.substr(resp.size() - 4), "\r\n\r\n");
}

// ═══════════════════════════════════════════════════════════════════════════════
// WebSocketFrame 辅助
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WebSocketTest, FrameIsControl) {
    WebSocketFrame f;
    f.opcode = WsOpcode::kText;
    EXPECT_FALSE(f.isControl());

    f.opcode = WsOpcode::kBinary;
    EXPECT_FALSE(f.isControl());

    f.opcode = WsOpcode::kClose;
    EXPECT_TRUE(f.isControl());

    f.opcode = WsOpcode::kPing;
    EXPECT_TRUE(f.isControl());

    f.opcode = WsOpcode::kPong;
    EXPECT_TRUE(f.isControl());
}
