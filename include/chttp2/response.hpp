#pragma once

#include <string>
#include <vector>

#include "chttp2/header_field.hpp"
#include "chttp2/http2_constants.hpp"

namespace chttp2 {

struct Response {
  std::vector<HeaderField> headers;
  std::vector<HeaderField> trailers;
  std::string body;

  bool isError{false};
  ErrorCode errorCode{ErrorCode::NO_ERROR};

  int getStatusCode() const {
    for (const auto& header : headers) {
      if (header.name == ":status") {
        return std::stoi(header.value);
      }
    }
    return -1;
  }

  std::string getBody() const { return body; }

  std::string getHeader(const std::string& key) const {
    for (const auto& header : headers) {
      if (header.name == key) {
        return header.value;
      }
    }
    return "";
  }

  std::string errorMsg;
  std::string extraMsg;

  static Response failConnectionError(const std::string& msg = "") {
    Response res;
    res.isError = true;
    res.errorMsg = "Connection error";
    res.extraMsg = msg;
    return res;
  }

  static Response failSendError() {
    Response res;
    res.isError = true;
    res.errorMsg = "Send error";
    return res;
  }

  static Response customError(const std::string& msg) {
    Response res;
    res.isError = true;
    res.errorMsg = msg;
    return res;
  }

  static Response protocolError(ErrorCode code, const std::string& msg = "") {
    Response res;
    res.isError = true;
    res.errorCode = code;
    res.errorMsg = "Protocol error";
    res.extraMsg = msg;
    return res;
  }
};

}  // namespace chttp2
