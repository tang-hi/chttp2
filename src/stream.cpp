#include "chttp2/stream.hpp"

namespace chttp2 {

Stream::Stream(StreamId id)
    : streamId(id),
      streamState(StreamState::IDLE),
      requestStarted(false),
      localEndStreamSent(false),
      remoteEndStreamReceived(false),
      waitingForContinuation(false),
      pendingHeaderEndStream(false),
      finalResponseHeadersReceived(false),
      trailingHeadersReceived(false),
      responseComplete(false),
      isHeadRequest(false),
      requestBodyOffset(0),
      expectedContentLength(-1),
      receivedContentLength(0),
      rqst(),
      resp(),
      lastEC(ErrorCode::NO_ERROR),
      inflow(),
      outflow() {}

Stream::~Stream() = default;

}  // namespace chttp2
