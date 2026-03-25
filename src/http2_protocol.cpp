#include "chttp2/http2_protocol.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "chttp2/frame.hpp"
#include "chttp2/hpack/hpack.hpp"
#include "chttp2/http2_constants.hpp"

namespace chttp2 {
namespace {

// ---------------------------------------------------------------------------
// Result merging
// ---------------------------------------------------------------------------

void mergeStepResult(ProtocolStepResult& dst, ProtocolStepResult&& src) {
  dst.outbound.insert(dst.outbound.end(),
                      std::make_move_iterator(src.outbound.begin()),
                      std::make_move_iterator(src.outbound.end()));
  dst.events.insert(dst.events.end(),
                    std::make_move_iterator(src.events.begin()),
                    std::make_move_iterator(src.events.end()));
  if (src.hasSubmittedStream) {
    dst.hasSubmittedStream = true;
    dst.submittedStreamId = src.submittedStreamId;
  }
}

// ---------------------------------------------------------------------------
// Request header encoding
// ---------------------------------------------------------------------------

std::vector<HeaderField> buildRequestHeaderFields(const Request& request) {
  std::vector<HeaderField> headers;
  headers.reserve(request.headers.size());
  for (const auto& header : request.headers) {
    headers.emplace_back(header.first, header.second);
  }
  return headers;
}

// ---------------------------------------------------------------------------
// String / validation helpers
// ---------------------------------------------------------------------------

std::string bufferToString(const ByteBuffer& buf) {
  return {reinterpret_cast<const char*>(buf.begin()), reinterpret_cast<const char*>(buf.end())};
}

// RFC 9113 §8.2.1: field name MUST NOT contain characters in the ranges
// 0x00-0x20, 0x41-0x5a (uppercase), or 0x7f-0xff.
bool hasInvalidFieldNameChar(const std::string& name) {
  return std::any_of(name.begin(), name.end(), [](char ch) {
    return ch <= 0x20 || (ch >= 0x41 && ch <= 0x5a) || ch >= 0x7f;
  });
}

// RFC 9113 §8.2.1: field value MUST NOT contain NUL (0x00), LF (0x0a),
// or CR (0x0d), and MUST NOT start or end with SP (0x20) or HTAB (0x09).
bool hasInvalidFieldValue(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  auto first = static_cast<unsigned char>(value.front());
  auto last = static_cast<unsigned char>(value.back());
  if (first == 0x20 || first == 0x09 || last == 0x20 || last == 0x09) {
    return true;
  }
  return std::any_of(value.begin(), value.end(), [](char ch) {
    auto c = static_cast<unsigned char>(ch);
    return c == 0x00 || c == 0x0a || c == 0x0d;
  });
}

bool equalsIgnoreCaseAscii(const std::string& lhs, const char* rhs) {
  std::size_t i = 0;
  for (; i < lhs.size() && rhs[i] != '\0'; ++i) {
    char lc = lhs[i];
    char rc = rhs[i];
    if (lc >= 'A' && lc <= 'Z') {
      lc = static_cast<char>(lc - 'A' + 'a');
    }
    if (rc >= 'A' && rc <= 'Z') {
      rc = static_cast<char>(rc - 'A' + 'a');
    }
    if (lc != rc) {
      return false;
    }
  }
  return i == lhs.size() && rhs[i] == '\0';
}

bool isConnectionSpecificHeader(const HeaderField& header) {
  return equalsIgnoreCaseAscii(header.name, "connection") ||
         equalsIgnoreCaseAscii(header.name, "proxy-connection") ||
         equalsIgnoreCaseAscii(header.name, "keep-alive") ||
         equalsIgnoreCaseAscii(header.name, "upgrade") ||
         equalsIgnoreCaseAscii(header.name, "transfer-encoding") ||
         equalsIgnoreCaseAscii(header.name, "te");
}

std::size_t headerListSize(const std::vector<HeaderField>& headers) {
  std::size_t total = 0;
  for (const auto& header : headers) {
    total += header.name.size() + header.value.size() + 32;
  }
  return total;
}

bool validatePaddedPayload(const RawFrame& frame,
                           std::uint32_t fixedPrefixBytes,
                           ErrorCode* errorCode,
                           std::string* debugData) {
  if (!frame.header.hasAnyFlag(FrameFlag::PADDED)) {
    if (frame.header.payloadLength < fixedPrefixBytes) {
      if (errorCode != nullptr) {
        *errorCode = ErrorCode::FRAME_SIZE_ERROR;
      }
      if (debugData != nullptr) {
        *debugData = "frame payload shorter than required prefix";
      }
      return false;
    }
    return true;
  }

  if (frame.header.payloadLength == 0) {
    if (errorCode != nullptr) {
      *errorCode = ErrorCode::PROTOCOL_ERROR;
    }
    if (debugData != nullptr) {
      *debugData = "PADDED frame missing pad length";
    }
    return false;
  }

  ByteSpan payload = frame.data;
  std::uint8_t padLength = payload[0];
  if (frame.header.payloadLength < 1u + fixedPrefixBytes) {
    if (errorCode != nullptr) {
      *errorCode = ErrorCode::FRAME_SIZE_ERROR;
    }
    if (debugData != nullptr) {
      *debugData = "frame payload shorter than padded prefix";
    }
    return false;
  }

  std::uint32_t remaining = frame.header.payloadLength - 1u - fixedPrefixBytes;
  if (padLength >= remaining) {
    if (errorCode != nullptr) {
      *errorCode = ErrorCode::PROTOCOL_ERROR;
    }
    if (debugData != nullptr) {
      *debugData = "padding exceeds frame payload";
    }
    return false;
  }
  return true;
}

bool tryParseStatusCode(const std::string& value, int* statusCode) {
  if (value.size() != 3) {
    return false;
  }
  int code = 0;
  for (char i : value) {
    if (i < '0' || i > '9') {
      return false;
    }
    code = (code * 10) + (i - '0');
  }
  if (code < 100 || code > 599) {
    return false;
  }
  if (statusCode != nullptr) {
    *statusCode = code;
  }
  return true;
}

bool tryParseContentLength(const std::string& value, int64_t* out) {
  if (value.empty()) {
    return false;
  }
  int64_t n = 0;
  for (char i : value) {
    if (i < '0' || i > '9') {
      return false;
    }
    if (n > 1000000000000000LL) {
      return false;
    }
    n = (n * 10) + static_cast<int64_t>(i - '0');
  }
  if (out != nullptr) {
    *out = n;
  }
  return true;
}

}  // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================

Http2Protocol::Http2Protocol() {
  reset();
}

Http2Protocol::Http2Protocol(const ClientConfig& config) {
  reset();
  maxConcurrentStreams = config.maxConcurrentStreams;
  decodeHeaderTableSize = config.headerTableSize;
  localMaxFrameSize = config.maxFrameSize;
  initialWindowSize = config.initialWindowSize;
  maxHeaderListSize = config.maxHeaderListSize;
  hpack.setDecodeTableLimit(config.headerTableSize);
  connectionInflow.init(static_cast<int32_t>(config.initialWindowSize));
}

Http2Protocol::~Http2Protocol() = default;

void Http2Protocol::reset() {
  frameCodec.clear();
  streams.clear();
  hpack.reset();

  maxConcurrentStreams = Http2SettingInfo::defaultMaxConcurrentStreams();
  encodeHeaderTableSize = Http2SettingInfo::defaultHeaderTableSize();
  decodeHeaderTableSize = Http2SettingInfo::defaultHeaderTableSize();
  maxFrameSize = Http2SettingInfo::defaultMaxFrameSize();
  localMaxFrameSize = Http2SettingInfo::defaultMaxFrameSize();
  initialWindowSize = Http2SettingInfo::defaultInitialWindowSize();
  maxHeaderListSize = Http2SettingInfo::defaultMaxHeaderListSize();
  enablePush = Http2SettingInfo::defaultEnablePush();

  hpack.setDecodeTableLimit(decodeHeaderTableSize);
  hpack.setEncodeTableSize(encodeHeaderTableSize);

  nextStreamId = 1;
  lastPeerStreamId = 0;
  started = false;
  closed = false;
  sentConnectionPreface = false;
  sentLocalSettings = false;
  gotPeerSettings = false;
  sentGoaway = false;
  receivedGoaway = false;
  transportClosed = false;
  expectingContinuation = false;
  continuationStreamId = 0;
  connectionErrorCode = ErrorCode::NO_ERROR;
  connectionDebugData.clear();

  connectionInflow = InFlow();
  connectionOutFlow = OutFlow();
  connectionInflow.init(static_cast<int32_t>(Http2SettingInfo::defaultInitialWindowSize()));
  connectionOutFlow.add(static_cast<int32_t>(Http2SettingInfo::defaultInitialWindowSize()));
}

// ===========================================================================
// Public API
// ===========================================================================

ProtocolStepResult Http2Protocol::start() {
  ProtocolStepResult result;

  if (started) {
    result.events.push_back(ProtocolEvent::connectionError(ErrorCode::PROTOCOL_ERROR));
    return result;
  }

  result.outbound.push_back(
      ByteBuffer::from(HTTP2_CONNECTION_PREFACE.begin(), HTTP2_CONNECTION_PREFACE.end()));
  sentConnectionPreface = true;

  appendOutboundFrame(&result, buildLocalSettingsFrame());
  sentLocalSettings = true;

  result.events.push_back(ProtocolEvent::connectionStarted());
  started = true;
  return result;
}

ProtocolStepResult Http2Protocol::submitRequest(const Request& request) {
  ProtocolStepResult result;

  if (!canSubmitRequests()) {
    result.events.push_back(ProtocolEvent::responseFailed(0, ErrorCode::REFUSED_STREAM));
    return result;
  }
  if (!request.isValid()) {
    result.events.push_back(ProtocolEvent::responseFailed(0, ErrorCode::PROTOCOL_ERROR));
    return result;
  }
  if (streams.size() >= maxConcurrentStreams) {
    result.events.push_back(ProtocolEvent::responseFailed(0, ErrorCode::REFUSED_STREAM));
    return result;
  }
  if (nextStreamId == 0 || nextStreamId > 0x7fffffffu - 1u) {
    result.events.push_back(ProtocolEvent::responseFailed(0, ErrorCode::REFUSED_STREAM));
    return result;
  }

  StreamId streamId = nextStreamId;
  nextStreamId += 2;

  std::unique_ptr<Stream> stream = std::make_unique<Stream>(streamId);
  stream->setRequest(request);
  stream->inflow.init(static_cast<int32_t>(initialWindowSize));
  stream->outflow.setConnFlow(&connectionOutFlow);
  stream->outflow.add(static_cast<int32_t>(initialWindowSize));
  Stream* streamPtr = stream.get();
  streams[streamId] = std::move(stream);

  result.events.push_back(ProtocolEvent::streamOpened(streamId));

  ByteBuffer encoded = hpack.encode(buildRequestHeaderFields(request));
  emitRequestHeaders(streamPtr, std::move(encoded), &result);

  streamPtr->requestStarted = true;
  streamPtr->streamState =
      streamPtr->localEndStreamSent ? StreamState::HALF_CLOSED_LOCAL : StreamState::OPEN;
  emitPendingRequestBody(streamPtr, &result);

  result.hasSubmittedStream = true;
  result.submittedStreamId = streamId;
  return result;
}

ProtocolStepResult Http2Protocol::receiveBytes(const std::uint8_t* data, std::size_t len) {
  ProtocolStepResult result;
  if (closed || transportClosed) {
    return result;
  }

  frameCodec.feed(data, len);

  RawFrame frame;
  while (frameCodec.tryPop(&frame)) {
    mergeStepResult(result, receiveFrame(frame));
    if (closed) {
      break;
    }
  }
  return result;
}

ProtocolStepResult Http2Protocol::cancelStream(StreamId streamId, ErrorCode errorCode) {
  if (closed) {
    return {};
  }
  return failStream(streamId, errorCode, /*sendRst=*/true);
}

ProtocolStepResult Http2Protocol::sendPing() {
  ProtocolStepResult result;
  if (closed || !started) {
    return result;
  }

  ++pingCounter;
  PingFrame frame;
  frame.header.type = FrameType::PING;
  frame.header.streamId = 0;
  frame.header.payloadLength = 8;
  // Store counter as 8-byte big-endian payload.
  std::uint8_t buf[8];
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<std::uint8_t>(pingCounter >> (8 * (7 - i)));
  }
  auto counterBytes = ByteBuffer::from(buf, buf + 8);
  frame.data = std::move(counterBytes);
  appendOutboundFrame(&result, frame);
  return result;
}

ProtocolStepResult Http2Protocol::close() {
  if (closed) {
    return {};
  }

  ProtocolStepResult result;

  if (!sentGoaway) {
    GoAwayFrame goaway = buildGoAwayFrame(ErrorCode::NO_ERROR, "");
    appendOutboundFrame(&result, goaway);
    result.events.push_back(
        ProtocolEvent::goAwaySent(goaway.lastStreamId, ErrorCode::NO_ERROR, ""));
    sentGoaway = true;
  }

  failAllStreams(ErrorCode::CANCEL, result);

  closed = true;
  result.events.push_back(ProtocolEvent::connectionClosed(ErrorCode::NO_ERROR));
  return result;
}

ProtocolStepResult Http2Protocol::onTransportClosed() {
  ProtocolStepResult result;
  transportClosed = true;

  if (!closed) {
    failAllStreams(ErrorCode::CANCEL, result);
  }

  closed = true;
  result.events.push_back(ProtocolEvent::transportClosed());
  result.events.push_back(ProtocolEvent::connectionClosed(connectionErrorCode));
  return result;
}

// ===========================================================================
// Frame dispatch
// ===========================================================================

ProtocolStepResult Http2Protocol::receiveFrame(const RawFrame& frame) {
  if (!gotPeerSettings && frame.header.getType() != FrameType::SETTINGS) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "first peer frame must be SETTINGS");
  }
  if (frame.header.payloadLength > localMaxFrameSize) {
    return failConnection(ErrorCode::FRAME_SIZE_ERROR, "frame exceeds local max frame size");
  }
  if (expectingContinuation && frame.header.getType() != FrameType::CONTINUATION) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "expected CONTINUATION");
  }

  switch (frame.header.getType()) {
    case FrameType::DATA:
      return processDataFrame(frame);
    case FrameType::HEADERS:
      return processHeadersFrame(frame);
    case FrameType::PRIORITY:
      return processPriorityFrame(frame);
    case FrameType::RST_STREAM:
      return processRstStreamFrame(frame);
    case FrameType::SETTINGS:
      return processSettingFrame(frame);
    case FrameType::PUSH_PROMISE:
      return processPushPromiseFrame(frame);
    case FrameType::PING:
      return processPingFrame(frame);
    case FrameType::GOAWAY:
      return processGoAwayFrame(frame);
    case FrameType::WINDOW_UPDATE:
      return processWindowUpdateFrame(frame);
    case FrameType::CONTINUATION:
      return processContinuationFrame(frame);
    default: {
      ProtocolStepResult result;
      result.events.push_back(ProtocolEvent::unknownFrameReceived(frame.header.type,
                                                                  frame.header.streamId,
                                                                  frame.header.flags,
                                                                  bufferToString(frame.data)));
      return result;
    }
  }
}

// ===========================================================================
// DATA
// ===========================================================================

ProtocolStepResult Http2Protocol::processDataFrame(const RawFrame& frame) {
  if (frame.header.streamId == 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "DATA on stream 0");
  }
  ErrorCode validationError = ErrorCode::NO_ERROR;
  std::string validationDebug;
  if (!validatePaddedPayload(frame, 0, &validationError, &validationDebug)) {
    return failConnection(validationError, validationDebug);
  }

  DataFrame dataFrame(frame);

  // Unknown / already-closed stream — still need to account for flow control.
  auto it = streams.find(frame.header.streamId);
  if (it == streams.end()) {
    return processDataOnUnknownStream(dataFrame);
  }

  Stream* stream = it->second.get();

  // --- Stream-level preconditions ---
  if (stream->isClosed() || stream->remoteEndStreamReceived) {
    return failStream(stream->streamId, ErrorCode::STREAM_CLOSED, /*sendRst=*/true);
  }
  if (!stream->finalResponseHeadersReceived) {
    return failStream(stream->streamId, ErrorCode::PROTOCOL_ERROR, /*sendRst=*/true);
  }
  if (!InFlow::takeInflows(&connectionInflow, &stream->inflow, dataFrame.header.payloadLength)) {
    return failConnection(ErrorCode::FLOW_CONTROL_ERROR, "flow control exceeded");
  }
  if (!dataFrame.data.empty() && stream->expectsNoBody()) {
    return failStream(stream->streamId, ErrorCode::PROTOCOL_ERROR, /*sendRst=*/true);
  }

  // --- Accumulate body ---
  stream->resp.body.append(dataFrame.data.begin(), dataFrame.data.end());
  stream->receivedContentLength += static_cast<int64_t>(dataFrame.data.size());

  // --- Build result and refresh flow-control windows ---
  ProtocolStepResult result;

  std::uint32_t consumed = dataFrame.header.payloadLength;

  int32_t connRefresh = connectionInflow.add(static_cast<int32_t>(consumed));
  if (connRefresh < 0) {
    return failConnection(ErrorCode::FLOW_CONTROL_ERROR, "connection inflow window overflow");
  }
  if (connRefresh > 0) {
    appendOutboundFrame(&result,
                        buildWindowUpdateFrame(0, static_cast<std::uint32_t>(connRefresh)));
  }

  int32_t streamRefresh = stream->inflow.add(static_cast<int32_t>(consumed));
  if (streamRefresh < 0) {
    return failStream(stream->streamId, ErrorCode::FLOW_CONTROL_ERROR, /*sendRst=*/true);
  }
  if (streamRefresh > 0) {
    appendOutboundFrame(
        &result,
        buildWindowUpdateFrame(stream->streamId, static_cast<std::uint32_t>(streamRefresh)));
  }

  if (dataFrame.header.hasEndStreamFlag()) {
    finalizeRemoteStream(stream, &result);
  }
  return result;
}

ProtocolStepResult Http2Protocol::processDataOnUnknownStream(const DataFrame& dataFrame) {
  StreamId streamId = dataFrame.header.streamId;

  if ((streamId & 1u) == 0u) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "unexpected peer-initiated DATA stream");
  }
  if (streamId >= nextStreamId) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "DATA for never-opened stream");
  }

  // Consume connection-level flow control even though the stream is gone.
  if (!connectionInflow.take(dataFrame.header.payloadLength)) {
    return failConnection(ErrorCode::FLOW_CONTROL_ERROR,
                          "old stream DATA exceeded connection window");
  }

  ProtocolStepResult result;
  int32_t connRefresh = connectionInflow.add(static_cast<int32_t>(dataFrame.header.payloadLength));
  if (connRefresh < 0) {
    return failConnection(ErrorCode::FLOW_CONTROL_ERROR, "connection inflow window overflow");
  }
  if (connRefresh > 0) {
    appendOutboundFrame(&result,
                        buildWindowUpdateFrame(0, static_cast<std::uint32_t>(connRefresh)));
  }
  return result;
}

// ===========================================================================
// HEADERS
// ===========================================================================

ProtocolStepResult Http2Protocol::processHeadersFrame(const RawFrame& frame) {
  if (frame.header.streamId == 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "HEADERS on stream 0");
  }
  ErrorCode validationError = ErrorCode::NO_ERROR;
  std::string validationDebug;
  std::uint32_t priorityPrefix = frame.header.hasAnyFlag(FrameFlag::PRIORITY) ? 5u : 0u;
  if (!validatePaddedPayload(frame, priorityPrefix, &validationError, &validationDebug)) {
    return failConnection(validationError, validationDebug);
  }

  HeadersFrame headersFrame(frame);

  // --- Unknown / already-closed stream: HPACK-decode to keep dynamic table in sync ---
  auto it = streams.find(frame.header.streamId);
  if (it == streams.end()) {
    if ((frame.header.streamId & 1u) == 0u) {
      return failConnection(ErrorCode::PROTOCOL_ERROR, "unexpected peer-initiated HEADERS stream");
    }
    if (frame.header.streamId >= nextStreamId) {
      return failConnection(ErrorCode::PROTOCOL_ERROR, "HEADERS for never-opened stream");
    }
    if (headersFrame.header.hasEndHeadersFlag()) {
      std::vector<HeaderField> discarded;
      if (!hpack.decode(headersFrame.data, discarded)) {
        return failConnection(ErrorCode::COMPRESSION_ERROR,
                              "HPACK decode failed on closed-stream HEADERS");
      }
    } else {
      ghostHeaderBlock = ByteBuffer(128u);
      ghostHeaderBlock.append(headersFrame.data.begin(), headersFrame.data.end());
      expectingContinuation = true;
      continuationStreamId = frame.header.streamId;
    }
    return {};
  }

  // --- Active stream ---
  Stream* stream = it->second.get();
  if (stream->isClosed() || stream->remoteEndStreamReceived) {
    return failStream(stream->streamId, ErrorCode::STREAM_CLOSED, /*sendRst=*/true);
  }

  stream->pendingHeaderBlock.append(headersFrame.data.begin(), headersFrame.data.end());
  if (maxHeaderListSize > 0 && stream->pendingHeaderBlock.size() > maxHeaderListSize) {
    return failConnection(ErrorCode::ENHANCE_YOUR_CALM, "header block too large");
  }

  if (!headersFrame.header.hasEndHeadersFlag()) {
    stream->waitingForContinuation = true;
    stream->pendingHeaderEndStream = headersFrame.header.hasEndStreamFlag();
    expectingContinuation = true;
    continuationStreamId = stream->streamId;
    return {};
  }

  // Complete header block — decode now.
  ProtocolStepResult result;
  ErrorCode errorCode =
      decodeResponseHeaders(stream, headersFrame.header.hasEndStreamFlag(), &result);
  if (errorCode != ErrorCode::NO_ERROR) {
    return handleHeaderBlockError(stream->streamId, errorCode);
  }
  return result;
}

// ===========================================================================
// PRIORITY / RST_STREAM / SETTINGS
// ===========================================================================

ProtocolStepResult Http2Protocol::processPriorityFrame(const RawFrame& frame) {
  if (frame.header.streamId == 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "PRIORITY on stream 0");
  }
  if (frame.header.payloadLength != 5) {
    ProtocolStepResult result;
    appendOutboundFrame(&result,
                        buildRstStreamFrame(frame.header.streamId, ErrorCode::FRAME_SIZE_ERROR));
    return result;
  }
  return {};
}

ProtocolStepResult Http2Protocol::processRstStreamFrame(const RawFrame& frame) {
  if (frame.header.streamId == 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "RST_STREAM on stream 0");
  }
  if (frame.header.payloadLength != 4) {
    return failConnection(ErrorCode::FRAME_SIZE_ERROR, "invalid RST_STREAM size");
  }

  auto it = streams.find(frame.header.streamId);
  if (it == streams.end()) {
    if (frame.header.streamId >= nextStreamId) {
      return failConnection(ErrorCode::PROTOCOL_ERROR, "RST_STREAM for idle stream");
    }
    return {};
  }

  RstStreamFrame rst(frame);
  auto errorCode = static_cast<ErrorCode>(rst.errorCode);

  ProtocolStepResult result;
  result.events.push_back(ProtocolEvent::streamResetReceived(frame.header.streamId, errorCode));
  mergeStepResult(result, failStream(frame.header.streamId, errorCode));
  return result;
}

ProtocolStepResult Http2Protocol::processSettingFrame(const RawFrame& frame) {
  if (frame.header.streamId != 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "SETTINGS with non-zero stream id");
  }
  if (frame.header.payloadLength % 6 != 0) {
    return failConnection(ErrorCode::FRAME_SIZE_ERROR, "invalid SETTINGS size");
  }

  ProtocolStepResult result;

  // --- ACK ---
  if (frame.header.hasAckFlag()) {
    if (frame.header.payloadLength != 0) {
      return failConnection(ErrorCode::FRAME_SIZE_ERROR, "SETTINGS ack must be empty");
    }
    result.events.push_back(ProtocolEvent::settingsAckReceived());
    return result;
  }

  // --- Apply peer settings ---
  gotPeerSettings = true;

  SettingFrame settingsFrame(frame);
  ErrorCode applyCode = applyPeerSettings(settingsFrame);
  if (applyCode != ErrorCode::NO_ERROR) {
    return failConnection(applyCode, "invalid peer settings");
  }

  // INITIAL_WINDOW_SIZE change may have enlarged stream outflow windows.
  for (auto& entry : streams) {
    emitPendingRequestBody(entry.second.get(), &result);
  }

  result.events.push_back(ProtocolEvent::settingsReceived());

  // Send ACK.
  SettingFrame ack;
  ack.header.type = FrameType::SETTINGS;
  ack.header.streamId = 0;
  ack.header.setAckFlag();
  ack.header.payloadLength = 0;
  appendOutboundFrame(&result, ack);
  return result;
}

// ===========================================================================
// PUSH_PROMISE / PING
// ===========================================================================

ProtocolStepResult Http2Protocol::processPushPromiseFrame(const RawFrame& frame) {
  if (frame.header.streamId == 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "PUSH_PROMISE on stream 0");
  }
  ErrorCode validationError = ErrorCode::NO_ERROR;
  std::string validationDebug;
  if (!validatePaddedPayload(frame, 4, &validationError, &validationDebug)) {
    return failConnection(validationError, validationDebug);
  }

  PushPromiseFrame push(frame);
  if (push.promisedStreamId == 0 || (push.promisedStreamId & 1u) != 0u) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "invalid promised stream id parity");
  }
  if (push.promisedStreamId <= lastPeerStreamId) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "promised stream id must be new");
  }

  ProtocolStepResult result;
  result.events.push_back(
      ProtocolEvent::pushPromiseReceived(frame.header.streamId, push.promisedStreamId));

  if (!enablePush) {
    mergeStepResult(result, failConnection(ErrorCode::PROTOCOL_ERROR, "server push disabled"));
  }
  return result;
}

ProtocolStepResult Http2Protocol::processPingFrame(const RawFrame& frame) {
  if (frame.header.streamId != 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "PING with non-zero stream id");
  }
  if (frame.header.payloadLength != 8) {
    return failConnection(ErrorCode::FRAME_SIZE_ERROR, "invalid PING size");
  }

  PingFrame ping(frame);
  ProtocolStepResult result;

  if (frame.header.hasAckFlag()) {
    result.events.push_back(ProtocolEvent::pingAckReceived(bufferToString(ping.data)));
    return result;
  }

  result.events.push_back(ProtocolEvent::pingReceived(bufferToString(ping.data)));

  PingFrame ack;
  ack.header.type = FrameType::PING;
  ack.header.streamId = 0;
  ack.header.setAckFlag();
  ack.header.payloadLength = 8;
  ack.data = ByteBuffer::from(ping.data.begin(), ping.data.end());
  appendOutboundFrame(&result, ack);
  return result;
}

// ===========================================================================
// GOAWAY
// ===========================================================================

ProtocolStepResult Http2Protocol::processGoAwayFrame(const RawFrame& frame) {
  if (frame.header.streamId != 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "GOAWAY with non-zero stream id");
  }
  if (frame.header.payloadLength < 8) {
    return failConnection(ErrorCode::FRAME_SIZE_ERROR, "invalid GOAWAY");
  }

  GoAwayFrame goaway(frame);
  auto errorCode = static_cast<ErrorCode>(goaway.errorCode);

  ProtocolStepResult result;
  receivedGoaway = true;
  result.events.push_back(
      ProtocolEvent::goAwayReceived(goaway.lastStreamId, errorCode, bufferToString(goaway.data)));

  // Fail streams above the last acknowledged stream ID.
  std::vector<StreamId> toFail;
  for (auto& entry : streams) {
    if (entry.first > goaway.lastStreamId) {
      toFail.push_back(entry.first);
    }
  }
  for (auto id : toFail) {
    mergeStepResult(result, failStream(id, errorCode));
  }

  return result;
}

// ===========================================================================
// WINDOW_UPDATE
// ===========================================================================

ProtocolStepResult Http2Protocol::processWindowUpdateFrame(const RawFrame& frame) {
  if (frame.header.payloadLength != 4) {
    return failConnection(ErrorCode::FRAME_SIZE_ERROR, "invalid WINDOW_UPDATE size");
  }

  WindowUpdateFrame window(frame);
  window.windowSizeIncrement &= 0x7fffffffu;

  // RFC 9113 §6.9: increment of 0 MUST be treated as error.
  if (window.windowSizeIncrement == 0) {
    if (frame.header.streamId == 0) {
      return failConnection(ErrorCode::PROTOCOL_ERROR, "zero connection window update");
    }
    return failStream(frame.header.streamId, ErrorCode::PROTOCOL_ERROR, /*sendRst=*/true);
  }

  ProtocolStepResult result;
  result.events.push_back(
      ProtocolEvent::windowUpdateReceived(frame.header.streamId, window.windowSizeIncrement));

  // --- Connection-level update: flush all streams that may have been blocked ---
  if (frame.header.streamId == 0) {
    if (!connectionOutFlow.add(static_cast<int32_t>(window.windowSizeIncrement))) {
      return failConnection(ErrorCode::FLOW_CONTROL_ERROR, "connection window overflow");
    }
    // Collect pointers — emitPendingRequestBody may erase streams.
    std::vector<Stream*> pending;
    pending.reserve(streams.size());
    for (auto& entry : streams) {
      pending.push_back(entry.second.get());
    }
    for (auto* s : pending) {
      if (streams.count(s->streamId)) {
        emitPendingRequestBody(s, &result);
      }
    }
    return result;
  }

  // --- Stream-level update ---
  auto it = streams.find(frame.header.streamId);
  if (it == streams.end()) {
    if (frame.header.streamId >= nextStreamId) {
      return failConnection(ErrorCode::PROTOCOL_ERROR, "WINDOW_UPDATE for idle stream");
    }
    return result;
  }
  if (!it->second->outflow.add(static_cast<int32_t>(window.windowSizeIncrement))) {
    return failStream(frame.header.streamId, ErrorCode::FLOW_CONTROL_ERROR, /*sendRst=*/true);
  }
  emitPendingRequestBody(it->second.get(), &result);
  return result;
}

// ===========================================================================
// CONTINUATION
// ===========================================================================

ProtocolStepResult Http2Protocol::processContinuationFrame(const RawFrame& frame) {
  if (frame.header.streamId == 0) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "CONTINUATION on stream 0");
  }
  if (!expectingContinuation || continuationStreamId != frame.header.streamId) {
    return failConnection(ErrorCode::PROTOCOL_ERROR, "unexpected CONTINUATION");
  }

  ContinuationFrame continuation(frame);
  auto it = streams.find(frame.header.streamId);

  // --- Ghost path: stream already removed, decode for HPACK table sync ---
  if (it == streams.end()) {
    ghostHeaderBlock.append(continuation.data.begin(), continuation.data.end());
    if (maxHeaderListSize > 0 && ghostHeaderBlock.size() > maxHeaderListSize) {
      ghostHeaderBlock.clear();
      return failConnection(ErrorCode::ENHANCE_YOUR_CALM, "header block too large");
    }
    if (!continuation.header.hasEndHeadersFlag()) {
      return {};
    }
    expectingContinuation = false;
    continuationStreamId = 0;
    std::vector<HeaderField> discarded;
    if (!hpack.decode(ghostHeaderBlock, discarded)) {
      ghostHeaderBlock.clear();
      return failConnection(ErrorCode::COMPRESSION_ERROR,
                            "HPACK decode failed on closed-stream CONTINUATION");
    }
    ghostHeaderBlock.clear();
    return {};
  }

  // --- Active stream: accumulate header block fragment ---
  Stream* stream = it->second.get();
  stream->pendingHeaderBlock.append(continuation.data.begin(), continuation.data.end());
  if (maxHeaderListSize > 0 && stream->pendingHeaderBlock.size() > maxHeaderListSize) {
    return failConnection(ErrorCode::ENHANCE_YOUR_CALM, "header block too large");
  }
  if (!continuation.header.hasEndHeadersFlag()) {
    return {};
  }

  // Complete header block.
  expectingContinuation = false;
  continuationStreamId = 0;
  stream->waitingForContinuation = false;

  ProtocolStepResult result;
  StreamId sid = stream->streamId;
  ErrorCode errorCode = decodeResponseHeaders(stream, stream->pendingHeaderEndStream, &result);
  if (errorCode != ErrorCode::NO_ERROR) {
    return handleHeaderBlockError(sid, errorCode);
  }
  return result;
}

// ===========================================================================
// Connection / stream error handling
// ===========================================================================

ProtocolStepResult Http2Protocol::failConnection(ErrorCode errorCode,
                                                 const std::string& debugData) {
  ProtocolStepResult result;

  if (!sentGoaway && !transportClosed) {
    GoAwayFrame goaway = buildGoAwayFrame(errorCode, debugData);
    appendOutboundFrame(&result, goaway);
    result.events.push_back(ProtocolEvent::goAwaySent(goaway.lastStreamId, errorCode, debugData));
    sentGoaway = true;
  }

  connectionErrorCode = errorCode;
  connectionDebugData = debugData;
  closed = true;
  expectingContinuation = false;
  continuationStreamId = 0;
  ghostHeaderBlock.clear();

  failAllStreams(errorCode, result);

  result.events.push_back(ProtocolEvent::connectionError(errorCode));
  result.events.push_back(ProtocolEvent::connectionClosed(errorCode));
  return result;
}

ProtocolStepResult Http2Protocol::failStream(StreamId streamId, ErrorCode errorCode, bool sendRst) {
  ProtocolStepResult result;

  auto it = streams.find(streamId);
  if (it == streams.end()) {
    return result;
  }

  if (sendRst && !closed && !transportClosed) {
    appendOutboundFrame(&result, buildRstStreamFrame(streamId, errorCode));
  }

  Stream* stream = it->second.get();

  // RFC 9113 §4.3: transfer pending header bytes to ghost buffer so
  // processContinuationFrame can complete the HPACK decode.
  if (stream->waitingForContinuation && !stream->pendingHeaderBlock.empty()) {
    ghostHeaderBlock = std::move(stream->pendingHeaderBlock);
  }

  stream->markClosed(errorCode);
  stream->responseComplete = true;
  result.events.push_back(ProtocolEvent::streamClosed(streamId, errorCode));
  result.events.push_back(ProtocolEvent::responseFailed(streamId, errorCode));
  streams.erase(it);
  return result;
}

void Http2Protocol::failAllStreams(ErrorCode errorCode, ProtocolStepResult& result) {
  std::vector<StreamId> ids;
  ids.reserve(streams.size());
  for (auto& entry : streams) {
    ids.push_back(entry.first);
  }
  for (auto id : ids) {
    mergeStepResult(result, failStream(id, errorCode));
  }
}

ProtocolStepResult Http2Protocol::handleHeaderBlockError(StreamId streamId, ErrorCode errorCode) {
  if (errorCode == ErrorCode::COMPRESSION_ERROR) {
    return failConnection(errorCode, "HPACK decode error");
  }
  return failStream(streamId, errorCode, /*sendRst=*/true);
}

// ===========================================================================
// Outbound frame emission
// ===========================================================================

void Http2Protocol::emitRequestHeaders(Stream* stream,
                                       ByteBuffer encoded,
                                       ProtocolStepResult* result) {
  bool hasBody = stream->hasBody();
  ByteSpan span = encoded;

  // Emit the HEADERS frame (always exactly one).
  {
    std::uint32_t chunkSize = std::min<std::uint32_t>(maxFrameSize, span.size());
    HeadersFrame frame(stream->streamId);
    if (chunkSize > 0) {
      frame.data = ByteBuffer::from(span.begin(), span.begin() + chunkSize);
      frame.header.payloadLength = chunkSize;
      span.advance(chunkSize);
    }
    if (span.empty()) {
      frame.header.setEndHeadersFlag();
      if (!hasBody) {
        frame.header.setEndStreamFlag();
        stream->localEndStreamSent = true;
      }
    }
    appendOutboundFrame(result, frame);
  }

  // Emit CONTINUATION frames for the remainder.
  while (!span.empty()) {
    std::uint32_t chunkSize = std::min<std::uint32_t>(maxFrameSize, span.size());
    ContinuationFrame frame(stream->streamId);
    frame.data = ByteBuffer::from(span.begin(), span.begin() + chunkSize);
    frame.header.payloadLength = chunkSize;
    span.advance(chunkSize);
    if (span.empty()) {
      frame.header.setEndHeadersFlag();
    }
    appendOutboundFrame(result, frame);
  }
}

void Http2Protocol::emitPendingRequestBody(Stream* stream, ProtocolStepResult* result) {
  if (stream == nullptr || result == nullptr || stream->localEndStreamSent || stream->isClosed()) {
    return;
  }

  while (stream->remainingBodySize()) {
    std::uint32_t chunkSize = static_cast<std::uint32_t>(
        std::min<std::size_t>(maxFrameSize, stream->remainingBodySize()));
    std::uint32_t available =
        static_cast<std::uint32_t>(std::max<int32_t>(0, stream->outflow.available()));
    chunkSize = std::min(chunkSize, available);
    if (chunkSize == 0) {
      break;
    }

    DataFrame frame(stream->streamId);
    frame.header.payloadLength = chunkSize;
    std::size_t beginOffset = stream->bodyOffset();
    std::size_t endOffset = beginOffset + chunkSize;
    auto bodyBegin = stream->rqst.body.begin();
    frame.data = ByteBuffer::from(bodyBegin + static_cast<std::ptrdiff_t>(beginOffset),
                                  bodyBegin + static_cast<std::ptrdiff_t>(endOffset));
    stream->outflow.take(static_cast<int32_t>(chunkSize));
    stream->requestBodyOffset = endOffset;
    if (stream->remainingBodySize() == 0) {
      frame.header.setEndStreamFlag();
      stream->localEndStreamSent = true;
    }
    appendOutboundFrame(result, frame);
  }

  if (stream->localEndStreamSent) {
    stream->streamState =
        stream->remoteEndStreamReceived ? StreamState::CLOSED : StreamState::HALF_CLOSED_LOCAL;
    if (stream->streamState == StreamState::CLOSED) {
      result->events.push_back(ProtocolEvent::streamClosed(stream->streamId, ErrorCode::NO_ERROR));
      streams.erase(stream->streamId);
    }
  }
}

// ===========================================================================
// Response header decoding and validation
// ===========================================================================

ErrorCode Http2Protocol::decodeResponseHeaders(Stream* stream,
                                               bool endStream,
                                               ProtocolStepResult* result) {
  if (stream == nullptr || result == nullptr) {
    return ErrorCode::INTERNAL_ERROR;
  }

  std::vector<HeaderField> headers;
  if (!hpack.decode(stream->pendingHeaderBlock, headers)) {
    return ErrorCode::COMPRESSION_ERROR;
  }
  if (maxHeaderListSize > 0 && headerListSize(headers) > maxHeaderListSize) {
    return ErrorCode::ENHANCE_YOUR_CALM;
  }

  stream->pendingHeaderBlock.clear();
  stream->waitingForContinuation = false;
  stream->pendingHeaderEndStream = false;

  bool informationalHeaders = false;
  ErrorCode validationCode =
      validateResponseHeaderBlock(stream, headers, endStream, &informationalHeaders);
  if (validationCode != ErrorCode::NO_ERROR) {
    return validationCode;
  }

  // 1xx informational — emit event but don't store on the stream.
  if (informationalHeaders) {
    Response informationalResponse;
    informationalResponse.headers = headers;
    result->events.push_back(
        ProtocolEvent::streamHeadersReceived(stream->streamId, informationalResponse));
    return ErrorCode::NO_ERROR;
  }

  if (!stream->finalResponseHeadersReceived) {
    // First non-informational header block: actual response headers.
    for (const auto& header : headers) {
      stream->resp.headers.push_back(header);
    }
    stream->finalResponseHeadersReceived = true;
  } else {
    // RFC 9113 §8.5: after a successful CONNECT response, the stream is a
    // tunnel — HEADERS (trailers) is a stream error.
    auto methodIt = stream->rqst.headers.find(":method");
    if (methodIt != stream->rqst.headers.end() && methodIt->second == "CONNECT") {
      return ErrorCode::PROTOCOL_ERROR;
    }
    // Subsequent header block: trailers.
    for (const auto& header : headers) {
      stream->resp.trailers.push_back(header);
    }
    stream->trailingHeadersReceived = true;
  }

  result->events.push_back(ProtocolEvent::streamHeadersReceived(stream->streamId, stream->resp));
  if (endStream) {
    finalizeRemoteStream(stream, result);
  }
  return ErrorCode::NO_ERROR;
}

ErrorCode Http2Protocol::validateResponseHeaderBlock(Stream* stream,
                                                     const std::vector<HeaderField>& headers,
                                                     bool endStream,
                                                     bool* informationalHeaders) const {
  if (stream == nullptr) {
    return ErrorCode::INTERNAL_ERROR;
  }
  if (informationalHeaders != nullptr) {
    *informationalHeaders = false;
  }
  if (headers.empty()) {
    return ErrorCode::PROTOCOL_ERROR;
  }

  bool sawRegularHeader = false;
  bool sawStatus = false;
  int statusCode = 0;
  for (const auto& header : headers) {
    if (header.name.empty() || hasInvalidFieldNameChar(header.name)) {
      return ErrorCode::PROTOCOL_ERROR;
    }
    if (hasInvalidFieldValue(header.value)) {
      return ErrorCode::PROTOCOL_ERROR;
    }
    if (isConnectionSpecificHeader(header)) {
      return ErrorCode::PROTOCOL_ERROR;
    }

    bool isPseudo = header.name[0] == ':';
    if (isPseudo) {
      if (sawRegularHeader) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      if (stream->finalResponseHeadersReceived) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      if (header.name != ":status") {
        return ErrorCode::PROTOCOL_ERROR;
      }
      if (sawStatus) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      if (!tryParseStatusCode(header.value, &statusCode)) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      sawStatus = true;
      continue;
    }

    sawRegularHeader = true;

    if (header.name.find(':') != std::string::npos) {
      return ErrorCode::PROTOCOL_ERROR;
    }

    if (!stream->finalResponseHeadersReceived &&
        equalsIgnoreCaseAscii(header.name, "content-length")) {
      int64_t cl = 0;
      if (!tryParseContentLength(header.value, &cl)) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      if (stream->expectedContentLength >= 0 && stream->expectedContentLength != cl) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      stream->expectedContentLength = cl;
    }
  }

  // --- Post-loop validation ---
  if (!stream->finalResponseHeadersReceived) {
    if (!sawStatus) {
      return ErrorCode::PROTOCOL_ERROR;
    }
    if ((statusCode / 100) == 1) {
      if (statusCode == 101) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      if (endStream) {
        return ErrorCode::PROTOCOL_ERROR;
      }
      if (informationalHeaders != nullptr) {
        *informationalHeaders = true;
      }
      return ErrorCode::NO_ERROR;
    }
    return ErrorCode::NO_ERROR;
  }

  // Trailers: no pseudo-headers allowed, must have END_STREAM.
  if (sawStatus) {
    return ErrorCode::PROTOCOL_ERROR;
  }
  if (!endStream) {
    return ErrorCode::PROTOCOL_ERROR;
  }
  return ErrorCode::NO_ERROR;
}

// ===========================================================================
// Stream finalization
// ===========================================================================

void Http2Protocol::finalizeRemoteStream(Stream* stream, ProtocolStepResult* result) {
  if (stream == nullptr || result == nullptr) {
    return;
  }

  stream->remoteEndStreamReceived = true;

  // Content-Length validation (HEAD, 204, 304 are exempt).
  if (stream->expectedContentLength >= 0 && !stream->expectsNoBody()) {
    if (stream->receivedContentLength != stream->expectedContentLength) {
      ProtocolStepResult fail =
          failStream(stream->streamId, ErrorCode::PROTOCOL_ERROR, /*sendRst=*/true);
      mergeStepResult(*result, std::move(fail));
      return;  // stream pointer is now dangling
    }
  }

  stream->responseComplete = true;
  result->events.push_back(
      ProtocolEvent::responseCompleted(stream->streamId, std::move(stream->resp)));

  if (stream->localEndStreamSent) {
    stream->streamState = StreamState::CLOSED;
    result->events.push_back(ProtocolEvent::streamClosed(stream->streamId, ErrorCode::NO_ERROR));
    streams.erase(stream->streamId);
  } else {
    stream->streamState = StreamState::HALF_CLOSED_REMOTE;
  }
}

// ===========================================================================
// SETTINGS helpers
// ===========================================================================

ErrorCode Http2Protocol::applyPeerSettings(const SettingFrame& frame) {
  for (const auto& setting : frame.settings) {
    std::uint32_t value = setting.second;
    switch (static_cast<Http2SettingParameter>(setting.first)) {
      case Http2SettingParameter::MAX_CONCURRENT_STREAMS:
        maxConcurrentStreams = value;
        break;

      case Http2SettingParameter::HEADER_TABLE_SIZE:
        encodeHeaderTableSize = value;
        hpack.setEncodeTableSize(value);
        break;

      case Http2SettingParameter::ENABLE_PUSH:
        // RFC 9113 §6.5.2 / §8.4: server MUST NOT set ENABLE_PUSH=1.
        if (value != 0) {
          return ErrorCode::PROTOCOL_ERROR;
        }
        enablePush = false;
        break;

      case Http2SettingParameter::MAX_FRAME_SIZE:
        if (value < Http2SettingInfo::minimumMaxFrameSize() ||
            value > Http2SettingInfo::maximumMaxFrameSize()) {
          return ErrorCode::PROTOCOL_ERROR;
        }
        maxFrameSize = value;
        break;

      case Http2SettingParameter::INITIAL_WINDOW_SIZE: {
        if (value > Http2SettingInfo::maximumWindowSize()) {
          return ErrorCode::FLOW_CONTROL_ERROR;
        }
        int delta = static_cast<int>(value) - static_cast<int>(initialWindowSize);
        initialWindowSize = value;
        for (auto& entry : streams) {
          if (!entry.second->outflow.add(delta)) {
            return ErrorCode::FLOW_CONTROL_ERROR;
          }
        }
      } break;

      case Http2SettingParameter::MAX_HEADER_LIST_SIZE:
        maxHeaderListSize = value;
        break;

      default:
        break;
    }
  }
  return ErrorCode::NO_ERROR;
}

// ===========================================================================
// Frame builders
// ===========================================================================

SettingFrame Http2Protocol::buildLocalSettingsFrame() const {
  SettingFrame frame;
  frame.header.type = FrameType::SETTINGS;
  frame.header.streamId = 0;
  frame.settings.emplace_back(
      static_cast<std::uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS),
      maxConcurrentStreams);
  frame.settings.emplace_back(static_cast<std::uint16_t>(Http2SettingParameter::HEADER_TABLE_SIZE),
                              decodeHeaderTableSize);
  frame.settings.emplace_back(static_cast<std::uint16_t>(Http2SettingParameter::ENABLE_PUSH),
                              enablePush ? 1u : 0u);
  frame.settings.emplace_back(static_cast<std::uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE),
                              localMaxFrameSize);
  frame.settings.emplace_back(
      static_cast<std::uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), initialWindowSize);
  // Only advertise MAX_HEADER_LIST_SIZE if explicitly configured (0 = no limit → omit).
  if (maxHeaderListSize > 0) {
    frame.settings.emplace_back(
        static_cast<std::uint16_t>(Http2SettingParameter::MAX_HEADER_LIST_SIZE), maxHeaderListSize);
  }
  frame.header.payloadLength = static_cast<std::uint32_t>(frame.settings.size() * 6);
  return frame;
}

GoAwayFrame Http2Protocol::buildGoAwayFrame(ErrorCode errorCode,
                                            const std::string& debugData) const {
  GoAwayFrame frame;
  frame.header.type = FrameType::GOAWAY;
  frame.header.streamId = 0;
  frame.lastStreamId = lastPeerStreamId;
  frame.errorCode = static_cast<std::uint32_t>(errorCode);
  frame.data = ByteBuffer::from(debugData.begin(), debugData.end());
  frame.header.payloadLength = 8 + frame.data.size();
  return frame;
}

RstStreamFrame Http2Protocol::buildRstStreamFrame(StreamId streamId, ErrorCode errorCode) const {
  RstStreamFrame frame(streamId);
  frame.header.type = FrameType::RST_STREAM;
  frame.header.streamId = streamId;
  frame.errorCode = static_cast<std::uint32_t>(errorCode);
  frame.header.payloadLength = 4;
  return frame;
}

WindowUpdateFrame Http2Protocol::buildWindowUpdateFrame(StreamId streamId,
                                                        std::uint32_t increment) const {
  WindowUpdateFrame frame(streamId);
  frame.header.type = FrameType::WINDOW_UPDATE;
  frame.windowSizeIncrement = increment;
  frame.header.payloadLength = 4;
  return frame;
}

}  // namespace chttp2
