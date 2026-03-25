#pragma once
#include <map>
#include <string>

namespace chttp2 {
struct Request {
  std::map<std::string, std::string> headers;
  std::string body;

  bool setHeader(const std::string& key, const std::string& value) {
    headers[key] = value;
    return true;
  }

  bool isValid() const {
    if (headers.empty()) {
      return false;
    }
    auto methodIt = headers.find(":method");
    if (methodIt == headers.end()) {
      return false;
    }
    bool isConnect = (methodIt->second == "CONNECT");
    if (isConnect) {
      // RFC 9113 §8.5: CONNECT MUST include :authority, MUST NOT
      // include :scheme or :path.
      if (headers.find(":authority") == headers.end()) {
        return false;
      }
      if (headers.find(":scheme") != headers.end()) {
        return false;
      }
      if (headers.find(":path") != headers.end()) {
        return false;
      }
    } else {
      // RFC 9113 §8.3.1: non-CONNECT requests MUST include :scheme
      // and :path pseudo-headers.
      if (headers.find(":scheme") == headers.end()) {
        return false;
      }
      if (headers.find(":path") == headers.end()) {
        return false;
      }
    }
    return true;
  }
};
};  // namespace chttp2
