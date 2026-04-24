#include "http/HttpResponse.h"
#include <sstream>

HttpResponse::HttpResponse(bool closeConnection)
    : closeConnection_(closeConnection) {}

void HttpResponse::setStatus(StatusCode code, const std::string &message) {
    statusCode_ = code;
    statusMessage_ = message;
}

void HttpResponse::addHeader(const std::string &key, const std::string &value) {
    headers_[key] = value;
}

void HttpResponse::setContentType(const std::string &contentType) {
    addHeader("Content-Type", contentType);
}

void HttpResponse::setBody(const std::string &body) { body_ = body; }
void HttpResponse::setBody(std::string &&body) { body_ = std::move(body); }

// 序列化：组装状态行 + 固定头部 + 用户头部 + 空行 + body
std::string HttpResponse::serialize() const {
    std::string result;
    result.reserve(256 + body_.size());

    // 状态行
    result += "HTTP/1.1 ";
    result += std::to_string(static_cast<int>(statusCode_));
    result += ' ';
    result += statusMessage_;
    result += "\r\n";

    // Content-Length 始终包含，让客户端无需等待 TCP 关闭就能确定响应体结束
    result += "Content-Length: ";
    result += std::to_string(body_.size());
    result += "\r\n";

    // 连接控制头
    if (closeConnection_) {
        result += "Connection: close\r\n";
    } else {
        result += "Connection: keep-alive\r\n";
    }

    // 用户自定义头部
    for (const auto &kv : headers_) {
        result += kv.first;
        result += ": ";
        result += kv.second;
        result += "\r\n";
    }

    // 空行
    result += "\r\n";

    // Body
    result += body_;

    return result;
}
