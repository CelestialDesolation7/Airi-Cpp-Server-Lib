// RpcMessage —— 二进制帧协议实现（Protobuf + Value 旁路透传版）
//
// 帧格式（16 字节定长头）：
//   [4B proto_section_len BE]   // = 4 + method.size() + payload.size()
//   [4B bypass_len BE]          // = bypass.size()（无旁路时为 0）
//   [4B msgType BE]
//   [4B reqId BE]
//   ── variable section (proto_section_len bytes) ──
//   [4B method_len BE] [method bytes] [payload bytes (Protobuf)]
//   ── bypass section (bypass_len bytes) ──
//   [raw value bytes]

#include "rpc/RpcMessage.h"
#include <arpa/inet.h>
#include <cstring>

std::string RpcMessage::encode() const {
    const uint32_t methodLen       = static_cast<uint32_t>(method.size());
    const uint32_t protoSectionLen = 4u + methodLen + static_cast<uint32_t>(payload.size());
    const uint32_t bypassLen       = static_cast<uint32_t>(bypass.size());

    std::string frame;
    frame.resize(16 + protoSectionLen + bypassLen);
    char *p = frame.data();

    const uint32_t netPSL   = htonl(protoSectionLen);
    const uint32_t netBPL   = htonl(bypassLen);
    const uint32_t netType  = htonl(static_cast<uint32_t>(type));
    const uint32_t netReqId = htonl(reqId);
    std::memcpy(p +  0, &netPSL,   4);
    std::memcpy(p +  4, &netBPL,   4);
    std::memcpy(p +  8, &netType,  4);
    std::memcpy(p + 12, &netReqId, 4);

    const uint32_t netML = htonl(methodLen);
    std::memcpy(p + 16, &netML, 4);
    if (methodLen  > 0) std::memcpy(p + 20,              method.data(),  methodLen);
    if (!payload.empty()) std::memcpy(p + 20 + methodLen, payload.data(), payload.size());
    if (!bypass.empty())  std::memcpy(p + 16 + protoSectionLen, bypass.data(), bypassLen);

    return frame;
}

bool RpcMessage::decode(const char *data, int len,
                        RpcMessage *out, int *consumed) {
    if (len < 16) return false;

    uint32_t netPSL, netBPL, netType, netReqId;
    std::memcpy(&netPSL,   data +  0, 4);
    std::memcpy(&netBPL,   data +  4, 4);
    std::memcpy(&netType,  data +  8, 4);
    std::memcpy(&netReqId, data + 12, 4);

    const uint32_t protoSectionLen = ntohl(netPSL);
    const uint32_t bypassLen       = ntohl(netBPL);

    // 防止整型溢出：先用 uint64_t 做加法
    const uint64_t totalNeeded64 = 16ULL + protoSectionLen + bypassLen;
    if (totalNeeded64 > static_cast<uint64_t>(INT32_MAX)) return false;
    const int totalNeeded = static_cast<int>(totalNeeded64);
    if (len < totalNeeded)   return false;
    if (protoSectionLen < 4) return false;  // 至少需要 method_len 字段

    out->type  = static_cast<RpcMessage::Type>(ntohl(netType));
    out->reqId = ntohl(netReqId);

    // 提取 method
    uint32_t netML = 0;
    std::memcpy(&netML, data + 16, 4);
    const uint32_t methodLen = ntohl(netML);
    if (4u + methodLen > protoSectionLen) return false;
    out->method.assign(data + 20, methodLen);

    // 提取 proto payload
    const uint32_t protoBodyLen = protoSectionLen - 4u - methodLen;
    if (protoBodyLen > 0)
        out->payload.assign(data + 20 + methodLen, protoBodyLen);
    else
        out->payload.clear();

    // 提取 bypass
    if (bypassLen > 0)
        out->bypass.assign(data + 16 + protoSectionLen, bypassLen);
    else
        out->bypass.clear();

    *consumed = totalNeeded;
    return true;
}
