#pragma once
// RpcMessage —— 帧协议数据结构
//
// 新帧格式（16B 定长头，替换旧 12B 头）：
//   [4B proto_section_len BE]   // 4 + method.size() + payload.size()
//   [4B bypass_len BE]          // bypass 字节数（无旁路透传时为 0）
//   [4B msgType BE]
//   [4B reqId BE]
//   [4B method_len BE] [method bytes]
//   [payload bytes]             // Protobuf 编码的结构化字段
//   [bypass bytes]              // 原始 value 字节（零序列化，仅 AppendEntries 使用）
//
// 设计要点：
//   · payload  = Protobuf 二进制，携带所有元数据字段
//   · bypass   = 原始 value 字节，完全绕过任何序列化编解码
//   · response 帧的 bypass 始终为空（Raft RPC 响应均为纯元数据）
//
#include <cstdint>
#include <string>

struct RpcMessage {
    enum class Type : uint32_t {
        kRequest  = 0,
        kResponse = 1,
        kOneWay   = 2,
    };

    Type        type{Type::kRequest};
    uint32_t    reqId{0};
    std::string method;
    std::string payload;  // Protobuf-encoded structured body
    std::string bypass;   // Raw value bytes（旁路透传，不经过任何序列化）

    std::string encode() const;
    static bool decode(const char *data, int len, RpcMessage *out, int *consumed);
};