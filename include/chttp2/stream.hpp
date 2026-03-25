#pragma once

#include <cstddef>
#include <cstdint>

#include "chttp2/byte_buffer.hpp"
#include "chttp2/flow.hpp"
#include "chttp2/http2_constants.hpp"
#include "chttp2/request.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

class Stream {
 public:
  explicit Stream(StreamId id);
  ~Stream();

  StreamId id() const { return streamId; }
  StreamState state() const { return streamState; }
  bool isClosed() const { return streamState == StreamState::CLOSED; }
  bool needsContinuation() const { return waitingForContinuation; }
  bool hasPendingRequestBody() const { return requestBodyOffset < rqst.body.size(); }
  bool hasLocalEndStream() const { return localEndStreamSent; }
  bool hasRemoteEndStream() const { return remoteEndStreamReceived; }
  bool isResponseComplete() const { return responseComplete; }

  bool hasBody() const { return !rqst.body.empty(); }
  std::size_t bodySize() const { return rqst.body.size(); }
  std::size_t bodyOffset() const { return requestBodyOffset; }
  std::size_t remainingBodySize() const { return rqst.body.size() - requestBodyOffset; }

  // True when the response semantics forbid a message body (HEAD, 204, 304).
  bool expectsNoBody() const {
    int status = resp.getStatusCode();
    return isHeadRequest || status == 204 || status == 304;
  }

  const Request& request() const { return rqst; }
  const Response& response() const { return resp; }
  ErrorCode lastErrorCode() const { return lastEC; }

  void setRequest(const Request& request) {
    rqst = request;
    auto it = rqst.headers.find(":method");
    isHeadRequest = (it != rqst.headers.end() && it->second == "HEAD");
  }
  void markClosed(ErrorCode errorCode) {
    lastEC = errorCode;
    streamState = StreamState::CLOSED;
  }

 private:
  friend class Http2Protocol;

  StreamId streamId;
  StreamState streamState;

  bool requestStarted;
  bool localEndStreamSent;
  bool remoteEndStreamReceived;
  bool waitingForContinuation;
  bool pendingHeaderEndStream;
  bool finalResponseHeadersReceived;
  bool trailingHeadersReceived;
  bool responseComplete;
  bool isHeadRequest;

  std::size_t requestBodyOffset;

  // RFC 9113 Section 8.1.1: Content-Length validation.
  // -1 means no Content-Length header was present; >= 0 is the declared size.
  int64_t expectedContentLength;
  int64_t receivedContentLength;

  Request rqst;
  Response resp;
  ErrorCode lastEC;
  ByteBuffer pendingHeaderBlock;

  InFlow inflow{};
  OutFlow outflow{};
};

}  // namespace chttp2
