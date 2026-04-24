#include "http/HttpRequest.h"

HttpRequest::HttpRequest() = default;

bool HttpRequest::setMethod(const std::string &m) {
    if (m == "GET")         { method_ = Method::kGet;    return true; }
    if (m == "POST")        { method_ = Method::kPost;   return true; }
    if (m == "HEAD")        { method_ = Method::kHead;   return true; }
    if (m == "PUT")         { method_ = Method::kPut;    return true; }
    if (m == "DELETE")      { method_ = Method::kDelete; return true; }
    method_ = Method::kInvalid;
    return false;
}

std::string HttpRequest::methodString() const {
    switch (method_) {
    case Method::kGet:    return "GET";
    case Method::kPost:   return "POST";
    case Method::kHead:   return "HEAD";
    case Method::kPut:    return "PUT";
    case Method::kDelete: return "DELETE";
    default:              return "INVALID";
    }
}

void HttpRequest::setVersion(const std::string &v) {
    if (v == "1.0")      version_ = Version::kHttp10;
    else if (v == "1.1") version_ = Version::kHttp11;
    else                 version_ = Version::kUnknown;
}

std::string HttpRequest::versionString() const {
    switch (version_) {
    case Version::kHttp10: return "HTTP/1.0";
    case Version::kHttp11: return "HTTP/1.1";
    default:               return "Unknown";
    }
}

void HttpRequest::addQueryParam(const std::string &key, const std::string &value) {
    queryParams_[key] = value;
}

std::string HttpRequest::queryParam(const std::string &key) const {
    auto it = queryParams_.find(key);
    return it != queryParams_.end() ? it->second : "";
}

void HttpRequest::addHeader(const std::string &key, const std::string &value) {
    headers_[key] = value;
}

std::string HttpRequest::header(const std::string &key) const {
    auto it = headers_.find(key);
    return it != headers_.end() ? it->second : "";
}

void HttpRequest::reset() {
    method_      = Method::kInvalid;
    version_     = Version::kUnknown;
    url_.clear();
    queryParams_.clear();
    headers_.clear();
    body_.clear();
}
