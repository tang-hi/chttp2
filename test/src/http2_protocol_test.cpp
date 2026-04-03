// Http2Protocol comprehensive unit tests
// Generated from docs/http2_protocol_test_plan.md
//
// Sections:
//   1.  Connection Lifecycle        [connection]
//   2.  SETTINGS                    [settings]
//   3.  HEADERS                     [headers]
//   4.  DATA                        [data]
//   5.  RST_STREAM                  [rst_stream]
//   6.  PING                        [ping]
//   7.  GOAWAY                      [goaway]
//   8.  WINDOW_UPDATE               [window_update]
//   9.  CONTINUATION                [continuation]
//   10. PUSH_PROMISE                [push_promise]
//   11. PRIORITY                    [priority]
//   12. Request Submission          [submit]
//   13. CancelStream                [cancel]
//   14. Response Header Validation  [response_headers]
//   15. Flow Control Integration    [flow_control]
//   16. Stream State Machine        [stream_state]
//   17. HPACK State Consistency     [hpack_state]
//   18. Half-Closed State           [half_closed]
//   19. Field Value Validation      [field_validation]
//   20. RST_STREAM Edge Cases       [rst_stream_edge]
//   21. Unknown Frames              [unknown_frame]
//   22. Error Handling              [error_handling]
//   23. Frame Size Enforcement      [frame_size]
//   24. Edge Cases / Robustness     [edge_cases]

#include "chttp2/http2_protocol.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "chttp2/frame.hpp"
#include "chttp2/frame_codec.hpp"
#include "chttp2/hpack/hpack.hpp"
#include "chttp2/http2_constants.hpp"
#include "chttp2/log.hpp"
#include "chttp2/protocol_event.hpp"
#include "chttp2/request.hpp"
#include "chttp2/response.hpp"
#include "chttp2/byte_buffer.hpp"

using namespace chttp2;
namespace {
struct LogSetup : Catch::EventListenerBase {
  using EventListenerBase::EventListenerBase;
  void testRunStarting(Catch::TestRunInfo const& /*testRunInfo*/) override {
    if (std::getenv("CHTTP2_LOG") == nullptr) {
      return;
    }
    setLogHandler([](LogLevel level, const char* msg) {
      const char* tag = "?";
      switch (level) {
        case LogLevel::DEBUG:
          tag = "DEBUG";
          break;
        case LogLevel::INFO:
          tag = "INFO";
          break;
        case LogLevel::WARN:
          tag = "WARN";
          break;
        case LogLevel::ERR:
          tag = "ERROR";
          break;
      }
      fprintf(stderr, "[%s] %s\n", tag, msg);
    });
  }
};
CATCH_REGISTER_LISTENER(LogSetup)
}  // namespace

// =============================================================================
// Test Infrastructure — helpers shared by every section
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// Frame builders — construct raw server-originated bytes for ReceiveBytes()
// ---------------------------------------------------------------------------

ByteBuffer makeServerSettings(
    const std::vector<std::pair<std::uint16_t, std::uint32_t> /*unused*/>& settings) {
  SettingFrame frame;
  frame.header.type = FrameType::SETTINGS;
  frame.header.streamId = 0;
  frame.settings = settings;
  frame.header.payloadLength = static_cast<std::uint32_t>(settings.size() * 6);
  return frame.toBytes();
}

ByteBuffer makeEmptyServerSettings() {
  return makeServerSettings({});
}

ByteBuffer makeSettingsAck() {
  SettingFrame frame;
  frame.header.type = FrameType::SETTINGS;
  frame.header.streamId = 0;
  frame.header.setAckFlag();
  frame.header.payloadLength = 0;
  return frame.toBytes();
}

ByteBuffer makeResponseHeaders(Hpack& encoder,
                          StreamId streamId,
                          int status,
                          const std::vector<HeaderField>& extra,
                          bool endStream,
                          bool endHeaders = true) {
  std::vector<HeaderField> headers;
  headers.emplace_back(":status", std::to_string(status));
  for (const auto& h : extra) {
    headers.push_back(h);
  }
  ByteBuffer encoded = encoder.encode(headers);
  HeadersFrame frame(streamId);
  frame.data = std::move(encoded);
  frame.header.payloadLength = frame.data.size();
  if (endHeaders) {
    frame.header.setEndHeadersFlag();
  }
  if (endStream) {
    frame.header.setEndStreamFlag();
  }
  return frame.toBytes();
}

ByteBuffer makeResponseHeadersRaw(const ByteBuffer& hpackBlock,
                             StreamId streamId,
                             bool endStream,
                             bool endHeaders = true,
                             bool priorityFlag = false) {
  HeadersFrame frame(streamId);
  frame.data = ByteBuffer::from(hpackBlock.begin(), hpackBlock.end());
  frame.header.payloadLength = frame.data.size();
  if (endHeaders) {
    frame.header.setEndHeadersFlag();
  }
  if (endStream) {
    frame.header.setEndStreamFlag();
  }
  if (priorityFlag) {
    frame.header.setPriorityFlag();
  }
  return frame.toBytes();
}

ByteBuffer makeDataFrame(StreamId streamId,
                    const std::string& payload,
                    bool endStream,
                    std::uint8_t padding = 0) {
  DataFrame frame(streamId);
  if (!payload.empty()) {
    frame.data = ByteBuffer::from(payload.begin(), payload.end());
  }
  if (padding > 0) {
    frame.header.setPaddedFlag();
    frame.padLength = padding;
    frame.padding = ByteBuffer(static_cast<std::uint32_t>(padding));
    for (std::uint32_t i = 0; i < padding; ++i) {
      frame.padding.append(static_cast<std::uint8_t>(0));
    }
    frame.header.payloadLength = 1 + static_cast<std::uint32_t>(payload.size()) + padding;
  } else {
    frame.header.payloadLength = static_cast<std::uint32_t>(payload.size());
  }
  if (endStream) {
    frame.header.setEndStreamFlag();
  }
  return frame.toBytes();
}

ByteBuffer makeRstStream(StreamId streamId, ErrorCode errorCode) {
  RstStreamFrame frame(streamId);
  frame.errorCode = static_cast<std::uint32_t>(errorCode);
  frame.header.payloadLength = 4;
  return frame.toBytes();
}

ByteBuffer makePing(const std::string& opaque8, bool isAck = false) {
  PingFrame frame;
  frame.header.streamId = 0;
  frame.header.payloadLength = 8;
  if (isAck) {
    frame.header.setAckFlag();
  }
  // exactly 8 bytes
  std::string padded = opaque8;
  padded.resize(8, '\0');
  frame.data = ByteBuffer::from(padded.begin(), padded.end());
  return frame.toBytes();
}

ByteBuffer makeGoAway(StreamId lastStreamId, ErrorCode errorCode, const std::string& debugData = "") {
  GoAwayFrame frame;
  frame.header.streamId = 0;
  frame.lastStreamId = lastStreamId;
  frame.errorCode = static_cast<std::uint32_t>(errorCode);
  frame.data = ByteBuffer::from(debugData.begin(), debugData.end());
  frame.header.payloadLength = 8 + frame.data.size();
  return frame.toBytes();
}

ByteBuffer makeWindowUpdate(StreamId streamId, std::uint32_t increment) {
  WindowUpdateFrame frame(streamId);
  frame.windowSizeIncrement = increment;
  frame.header.payloadLength = 4;
  return frame.toBytes();
}

ByteBuffer makeContinuation(StreamId streamId, const ByteBuffer& hpackBlock, bool endHeaders) {
  ContinuationFrame frame(streamId);
  frame.data = ByteBuffer::from(hpackBlock.begin(), hpackBlock.end());
  frame.header.payloadLength = frame.data.size();
  if (endHeaders) {
    frame.header.setEndHeadersFlag();
  }
  return frame.toBytes();
}

ByteBuffer makePushPromise(StreamId streamId, StreamId promisedStreamId, const ByteBuffer& hpackBlock) {
  PushPromiseFrame frame(streamId);
  frame.promisedStreamId = promisedStreamId;
  frame.data = ByteBuffer::from(hpackBlock.begin(), hpackBlock.end());
  frame.header.payloadLength = 4 + frame.data.size();
  return frame.toBytes();
}

ByteBuffer makePriority(StreamId streamId,
                   StreamId dependency,
                   std::uint8_t weight,
                   bool exclusive = false) {
  // PRIORITY: 5 bytes = 4-byte dependency (with E bit) + 1-byte weight
  ByteBuffer payload = ByteBuffer(8u);
  std::uint32_t dep = dependency;
  if (exclusive) {
    dep |= 0x80000000u;
  }
  payload.append<std::uint32_t>(dep);
  payload.append<std::uint8_t>(weight);
  // Build raw frame
  FrameHeader hdr;
  hdr.type = FrameType::PRIORITY;
  hdr.streamId = streamId;
  hdr.flags = 0;
  hdr.payloadLength = 5;
  ByteBuffer headerBytes = FrameHeader::encode(hdr);
  ByteBuffer result = ByteBuffer(14u);
  result.append(headerBytes.begin(), headerBytes.end());
  result.append(payload.begin(), payload.end());
  return result;
}

ByteBuffer makeRawFrame(FrameType type, Flags flags, StreamId streamId, const ByteBuffer& payload) {
  FrameHeader hdr;
  hdr.type = type;
  hdr.flags = flags;
  hdr.streamId = streamId;
  hdr.payloadLength = payload.size();
  ByteBuffer headerBytes = FrameHeader::encode(hdr);
  ByteBuffer result(static_cast<std::uint32_t>(9 + payload.size()));
  result.append(headerBytes.begin(), headerBytes.end());
  result.append(payload.begin(), payload.end());
  return result;
}

ByteBuffer makeRawFrame(FrameType type, Flags flags, StreamId streamId, const std::string& payload) {
  ByteBuffer p = ByteBuffer::from(payload.begin(), payload.end());
  return makeRawFrame(type, flags, streamId, p);
}

// ---------------------------------------------------------------------------
// Outbound frame decoder
// ---------------------------------------------------------------------------

std::vector<RawFrame> decodeOutbound(const ProtocolStepResult& result) {
  std::vector<RawFrame> frames;
  FrameCodec codec;
  for (const auto& slice : result.outbound) {
    codec.feed(slice);
  }
  RawFrame raw;
  while (codec.tryPop(&raw)) {
    frames.push_back(std::move(raw));
  }
  return frames;
}

// ---------------------------------------------------------------------------
// Event query helpers
// ---------------------------------------------------------------------------

bool hasEvent(const ProtocolStepResult& r, ProtocolEventType type) {
  return std::any_of(
      r.events.begin(), r.events.end(), [type](const ProtocolEvent& e) { return e.type == type; });
}

const ProtocolEvent& findEvent(const ProtocolStepResult& r, ProtocolEventType type) {
  for (const auto& e : r.events) {
    if (e.type == type) {
      return e;
    }
  }
  FAIL("Expected event not found: " + std::to_string(static_cast<int>(type)));

  // unreachable, but satisfy compiler
  static ProtocolEvent dummy;
  return dummy;
}

std::vector<ProtocolEvent> findEvents(const ProtocolStepResult& r, ProtocolEventType type) {
  std::vector<ProtocolEvent> out;
  for (const auto& e : r.events) {
    if (e.type == type) {
      out.push_back(e);
    }
  }
  return out;
}

std::size_t countEvents(const ProtocolStepResult& r, ProtocolEventType type) {
  return findEvents(r, type).size();
}

// Merge multiple ProtocolStepResult into one (for multi-step feeding)

// ---------------------------------------------------------------------------
// Feed helper — wraps ReceiveBytes for a ByteBuffer
// ---------------------------------------------------------------------------

ProtocolStepResult feed(Http2Protocol& proto, const ByteBuffer& data) {
  return proto.receiveBytes(data.begin(), static_cast<std::size_t>(data.size()));
}

// ---------------------------------------------------------------------------
// Handshake helper — brings protocol past the initial handshake
// ---------------------------------------------------------------------------

struct HandshakeResult {
  ProtocolStepResult startResult;
  ProtocolStepResult settingsResult;
  ProtocolStepResult ackResult;
};

HandshakeResult handshake(
    Http2Protocol& proto,
    const std::vector<std::pair<std::uint16_t, std::uint32_t> /*unused*/>& serverSettings = {}) {
  HandshakeResult hr;
  hr.startResult = proto.start();
  ByteBuffer settingsFrame =
      serverSettings.empty() ? makeEmptyServerSettings() : makeServerSettings(serverSettings);
  hr.settingsResult = feed(proto, settingsFrame);
  hr.ackResult = feed(proto, makeSettingsAck());
  return hr;
}

// ---------------------------------------------------------------------------
// Submit helpers
// ---------------------------------------------------------------------------

Request makeGetRequest(const std::string& path = "/") {
  Request req;
  req.headers[":method"] = "GET";
  req.headers[":scheme"] = "https";
  req.headers[":path"] = path;
  req.headers[":authority"] = "example.com";
  return req;
}

Request makePostRequest(const std::string& body, const std::string& path = "/") {
  Request req;
  req.headers[":method"] = "POST";
  req.headers[":scheme"] = "https";
  req.headers[":path"] = path;
  req.headers[":authority"] = "example.com";
  req.body = body;
  return req;
}

Request makeHeadRequest(const std::string& path = "/") {
  Request req;
  req.headers[":method"] = "HEAD";
  req.headers[":scheme"] = "https";
  req.headers[":path"] = path;
  req.headers[":authority"] = "example.com";
  return req;
}

StreamId submitSimpleGet(Http2Protocol& proto, const std::string& path = "/") {
  auto result = proto.submitRequest(makeGetRequest(path));
  REQUIRE(result.hasSubmittedStream);
  return result.submittedStreamId;
}

// Complete a simple 200 response with optional body
void completeResponse(Http2Protocol& proto,
                      Hpack& encoder,
                      StreamId streamId,
                      int status = 200,
                      const std::string& body = "",
                      const std::vector<HeaderField>& trailers = {}) {
  bool hasBody = !body.empty();
  bool hasTrailers = !trailers.empty();
  bool endStreamOnHeaders = !hasBody && !hasTrailers;

  ByteBuffer headersFrame = makeResponseHeaders(encoder, streamId, status, {}, endStreamOnHeaders);
  feed(proto, headersFrame);

  if (hasBody) {
    bool endStreamOnData = !hasTrailers;
    ByteBuffer dataFrame = makeDataFrame(streamId, body, endStreamOnData);
    feed(proto, dataFrame);
  }

  if (hasTrailers) {
    const std::vector<HeaderField>& trailerFields = trailers;
    ByteBuffer encoded = encoder.encode(trailerFields);
    HeadersFrame tf(streamId);
    tf.data = std::move(encoded);
    tf.header.payloadLength = tf.data.size();
    tf.header.setEndHeadersFlag();
    tf.header.setEndStreamFlag();
    ByteBuffer trailerSlice = tf.toBytes();
    feed(proto, trailerSlice);
  }
}

// Check connection error pattern.
// expected_last_stream_id: if non-negative, verify GOAWAY.lastStreamId matches.
// For a client that only receives server-initiated streams via PUSH_PROMISE
// (which is disabled), this is typically 0.
void requireConnectionError(const ProtocolStepResult& r,
                            ErrorCode expectedCode,
                            const Http2Protocol& proto,
                            int64_t expectedLastStreamId = -1) {
  REQUIRE(hasEvent(r, ProtocolEventType::CONNECTION_ERROR));
  auto err = findEvent(r, ProtocolEventType::CONNECTION_ERROR);
  REQUIRE(err.errorCode == expectedCode);
  REQUIRE(hasEvent(r, ProtocolEventType::CONNECTION_CLOSED));
  REQUIRE(proto.isClosed());

  // Should have GOAWAY in outbound
  auto frames = decodeOutbound(r);
  bool foundGoaway = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::GOAWAY) {
      foundGoaway = true;
      GoAwayFrame ga(f);
      REQUIRE(static_cast<ErrorCode>(ga.errorCode) == expectedCode);
      if (expectedLastStreamId >= 0) {
        REQUIRE(ga.lastStreamId == static_cast<StreamId>(expectedLastStreamId));
      }
    }
  }
  REQUIRE(foundGoaway);
}

// Check stream error pattern: RST_STREAM sent, RESPONSE_FAILED emitted,
// connection stays alive.
void requireStreamError(const ProtocolStepResult& r,
                        ErrorCode expectedCode,
                        StreamId streamId,
                        const Http2Protocol& proto) {
  REQUIRE_FALSE(proto.isClosed());
  REQUIRE_FALSE(hasEvent(r, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(r, ProtocolEventType::RESPONSE_FAILED));

  auto frames = decodeOutbound(r);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == streamId) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == expectedCode);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
}

}  // anonymous namespace

// =============================================================================
// Section 1: Connection Lifecycle [connection]
// =============================================================================

TEST_CASE("1.1.1 Start emits connection preface and SETTINGS", "[connection]") {
  Http2Protocol proto;
  auto result = proto.start();

  // First outbound slice is the 24-byte connection preface
  REQUIRE(result.outbound.size() >= 2);
  REQUIRE(result.outbound[0].size() == HTTP2_CONNECTION_PREFACE.size());
  REQUIRE(std::equal(
      result.outbound[0].begin(), result.outbound[0].end(), HTTP2_CONNECTION_PREFACE.begin()));

  // Second outbound is a SETTINGS frame
  auto frames = decodeOutbound(result);
  // The first decodable frame (after the preface) should be SETTINGS
  // The preface is not a frame, so DecodeOutbound skips it — we just check
  // that the second slice decodes as SETTINGS.
  FrameCodec codec;
  codec.feed(result.outbound[1]);
  RawFrame raw;
  REQUIRE(codec.tryPop(&raw));
  REQUIRE(raw.header.getType() == FrameType::SETTINGS);
  REQUIRE(raw.header.streamId == 0);
  REQUIRE(raw.header.payloadLength % 6 == 0);

  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_STARTED));
  REQUIRE(proto.isStarted());
}

TEST_CASE("1.1.2 Start emits all local settings", "[connection]") {
  Http2Protocol proto;
  auto result = proto.start();

  FrameCodec codec;
  codec.feed(result.outbound[1]);
  RawFrame raw;
  REQUIRE(codec.tryPop(&raw));
  SettingFrame sf(raw);

  // Should include 6 settings
  REQUIRE(sf.settings.size() == 6);
  std::vector<std::uint16_t> ids;
  ids.reserve(sf.settings.size());
  for (auto& s : sf.settings) {
    ids.push_back(s.first);
  }

  auto hasId = [&](Http2SettingParameter p) {
    auto v = static_cast<std::uint16_t>(p);
    return std::find(ids.begin(), ids.end(), v) != ids.end();
  };
  REQUIRE(hasId(Http2SettingParameter::HEADER_TABLE_SIZE));
  REQUIRE(hasId(Http2SettingParameter::ENABLE_PUSH));
  REQUIRE(hasId(Http2SettingParameter::MAX_CONCURRENT_STREAMS));
  REQUIRE(hasId(Http2SettingParameter::MAX_FRAME_SIZE));
  REQUIRE(hasId(Http2SettingParameter::INITIAL_WINDOW_SIZE));
  REQUIRE(hasId(Http2SettingParameter::MAX_HEADER_LIST_SIZE));
}

TEST_CASE("1.1.3 Start can be called after Close for reconnection", "[connection]") {
  Http2Protocol proto;
  handshake(proto);
  proto.close();
  proto.reset();

  auto result = proto.start();
  REQUIRE(proto.isStarted());
  REQUIRE_FALSE(proto.isClosed());
  REQUIRE(proto.nextLocalStreamId() == 1);
  REQUIRE(proto.activeStreamCount() == 0);
  REQUIRE(result.outbound.size() >= 2);
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_STARTED));
}

TEST_CASE("1.1.4 Double Start without Reset triggers error", "[connection]") {
  Http2Protocol proto;
  proto.start();

  // Calling start() again without reset() should produce a connection error
  auto result = proto.start();
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  auto err = findEvent(result, ProtocolEventType::CONNECTION_ERROR);
  REQUIRE(err.errorCode == ErrorCode::PROTOCOL_ERROR);
}

TEST_CASE("1.2.1 Close emits GOAWAY with NO_ERROR", "[connection]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.close();
  REQUIRE(proto.isClosed());

  auto frames = decodeOutbound(result);
  bool foundGoaway = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::GOAWAY) {
      GoAwayFrame ga(f);
      REQUIRE(static_cast<ErrorCode>(ga.errorCode) == ErrorCode::NO_ERROR);
      REQUIRE(ga.lastStreamId == 0);
      foundGoaway = true;
    }
  }
  REQUIRE(foundGoaway);
  REQUIRE(hasEvent(result, ProtocolEventType::GOAWAY_SENT));
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_CLOSED));
}

TEST_CASE("1.2.2 Close with active streams fails them all", "[connection]") {
  Http2Protocol proto;
  handshake(proto);
  submitSimpleGet(proto, "/1");
  submitSimpleGet(proto, "/2");
  submitSimpleGet(proto, "/3");
  REQUIRE(proto.activeStreamCount() == 3);

  auto result = proto.close();
  REQUIRE(proto.activeStreamCount() == 0);

  auto fails = findEvents(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails.size() == 3);
  for (auto& f : fails) {
    REQUIRE(f.errorCode == ErrorCode::CANCEL);
  }
}

// Test 1.2.3 removed: strict subset of test 1.2.4

TEST_CASE("1.2.4 Close sends only one GOAWAY", "[connection]") {
  Http2Protocol proto;
  handshake(proto);
  auto r1 = proto.close();
  auto r2 = proto.close();

  auto frames1 = decodeOutbound(r1);
  auto frames2 = decodeOutbound(r2);
  int goawayCount = 0;
  for (auto& f : frames1) {
    if (f.header.getType() == FrameType::GOAWAY) {
      goawayCount++;
    }
  }
  for (auto& f : frames2) {
    if (f.header.getType() == FrameType::GOAWAY) {
      goawayCount++;
    }
  }
  REQUIRE(goawayCount == 1);
}

TEST_CASE("1.3.1 OnTransportClosed cleans up active streams", "[connection]") {
  Http2Protocol proto;
  handshake(proto);
  submitSimpleGet(proto, "/1");
  submitSimpleGet(proto, "/2");

  auto result = proto.onTransportClosed();
  REQUIRE(proto.isClosed());
  REQUIRE(proto.activeStreamCount() == 0);

  auto fails = findEvents(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails.size() == 2);
  REQUIRE(hasEvent(result, ProtocolEventType::TRANSPORT_CLOSED));
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_CLOSED));
}

TEST_CASE("1.3.2 OnTransportClosed does not emit GOAWAY", "[connection]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.onTransportClosed();
  auto frames = decodeOutbound(result);
  for (auto& f : frames) {
    REQUIRE(f.header.getType() != FrameType::GOAWAY);
  }
}

TEST_CASE("1.3.3 ReceiveBytes after OnTransportClosed is no-op", "[connection]") {
  Http2Protocol proto;
  handshake(proto);
  proto.onTransportClosed();

  ByteBuffer settings = makeEmptyServerSettings();
  auto result = feed(proto, settings);
  REQUIRE(result.outbound.empty());
  REQUIRE(result.events.empty());
}

// =============================================================================
// Section 2: SETTINGS [settings]
// =============================================================================

TEST_CASE("2.1.1 Receive server SETTINGS and reply ACK", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 100}});
  auto result = feed(proto, settings);

  // Should have SETTINGS ACK in outbound
  auto frames = decodeOutbound(result);
  bool foundAck = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::SETTINGS && f.header.hasAckFlag()) {
      REQUIRE(f.header.payloadLength == 0);
      foundAck = true;
    }
  }
  REQUIRE(foundAck);
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));

  REQUIRE(proto.peerMaxConcurrentStreams() == 100);
}

TEST_CASE("2.1.2 Receive SETTINGS ACK", "[settings]") {
  Http2Protocol proto;
  proto.start();
  // First feed server settings so protocol accepts further frames
  feed(proto, makeEmptyServerSettings());

  auto result = feed(proto, makeSettingsAck());
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_ACK_RECEIVED));
  REQUIRE(result.outbound.empty());
}

TEST_CASE("2.1.3 First frame must be SETTINGS", "[settings]") {
  Http2Protocol proto;
  proto.start();

  // Feed DATA as first frame
  ByteBuffer data = makeDataFrame(1, "hello", false);
  auto result = feed(proto, data);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

// Tests 2.1.4, 2.1.5 removed: same core logic as 2.1.3 (first frame must be SETTINGS)

TEST_CASE("2.2.1 SETTINGS on non-zero stream", "[settings]") {
  Http2Protocol proto;
  proto.start();

  // Forge a SETTINGS frame with streamId=1
  ByteBuffer emptyPayload;
  ByteBuffer raw = makeRawFrame(FrameType::SETTINGS, 0, 1, std::string());
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("2.2.2 SETTINGS payload not multiple of 6", "[settings]") {
  Http2Protocol proto;
  proto.start();

  // 7 bytes of payload
  std::string payload(7, '\0');
  ByteBuffer raw = makeRawFrame(FrameType::SETTINGS, 0, 0, payload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

TEST_CASE("2.2.3 SETTINGS ACK with non-empty payload", "[settings]") {
  Http2Protocol proto;
  proto.start();
  feed(proto, makeEmptyServerSettings());

  // ACK flag (0x1) + 6 bytes payload
  std::string payload(6, '\0');
  ByteBuffer raw = makeRawFrame(FrameType::SETTINGS, static_cast<Flags>(FrameFlag::ACK), 0, payload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

TEST_CASE("2.2.4 ENABLE_PUSH=1 from server", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings =
      makeServerSettings({{static_cast<uint16_t>(Http2SettingParameter::ENABLE_PUSH), 1}});
  auto result = feed(proto, settings);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("2.2.4.2 ENABLE_PUSH=0 from server accepted", "[settings]") {
  // RFC 9113 §6.5.2: server MAY explicitly send ENABLE_PUSH=0.
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings =
      makeServerSettings({{static_cast<uint16_t>(Http2SettingParameter::ENABLE_PUSH), 0}});
  auto result = feed(proto, settings);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));
}

TEST_CASE("2.2.5 ENABLE_PUSH=2 (invalid value)", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings =
      makeServerSettings({{static_cast<uint16_t>(Http2SettingParameter::ENABLE_PUSH), 2}});
  auto result = feed(proto, settings);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("2.2.6 MAX_FRAME_SIZE below minimum (16383)", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings =
      makeServerSettings({{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), 16383}});
  auto result = feed(proto, settings);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("2.2.7 MAX_FRAME_SIZE above maximum (2^24)", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), (1u << 24)}});
  auto result = feed(proto, settings);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("2.2.8 INITIAL_WINDOW_SIZE exceeds 2^31-1", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 0x80000000u}});
  auto result = feed(proto, settings);
  requireConnectionError(result, ErrorCode::FLOW_CONTROL_ERROR, proto);
}

TEST_CASE("2.2.9 Unknown setting ID ignored", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings = makeServerSettings({{0xFF, 42}});
  auto result = feed(proto, settings);
  // No error, setting silently ignored
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));
}

TEST_CASE("2.3.1 MAX_CONCURRENT_STREAMS limits stream creation", "[settings]") {
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 1}});

  auto r1 = proto.submitRequest(makeGetRequest("/1"));
  REQUIRE(r1.hasSubmittedStream);

  auto r2 = proto.submitRequest(makeGetRequest("/2"));
  REQUIRE_FALSE(r2.hasSubmittedStream);
  REQUIRE(hasEvent(r2, ProtocolEventType::RESPONSE_FAILED));
  auto fail = findEvent(r2, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fail.errorCode == ErrorCode::REFUSED_STREAM);
}

TEST_CASE("2.3.2 INITIAL_WINDOW_SIZE change adjusts existing streams", "[settings]") {
  Http2Protocol proto;
  // Start with small initial window
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 5}});

  // Submit POST with body larger than window
  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  REQUIRE(submitResult.hasSubmittedStream);

  // Check that only 5 bytes of body were sent
  auto frames = decodeOutbound(submitResult);
  std::uint32_t dataBytes = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes == 5);

  // Now server increases window
  ByteBuffer settingsUpdate = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100}});
  auto result = feed(proto, settingsUpdate);

  // The remaining body should now be emitted
  auto frames2 = decodeOutbound(result);
  std::uint32_t dataBytes2 = 0;
  for (auto& f : frames2) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes2 += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes2 == 5);  // remaining 5 bytes
}

TEST_CASE("2.3.3 INITIAL_WINDOW_SIZE change causes stream window overflow", "[settings]") {
  // RFC 9113 §6.9.2: if a SETTINGS_INITIAL_WINDOW_SIZE change causes any
  // stream's flow-control window to exceed 2^31-1, this is a connection error
  // of type FLOW_CONTROL_ERROR.
  //
  // Note: test 2.2.8 already covers the case where the SETTINGS value itself
  // exceeds 2^31-1.  THIS test verifies the arithmetic overflow path: the
  // value is valid, but the delta applied to an inflated stream window overflows.
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 1}});

  // Submit POST with large body so stream stays open (only 1 byte sent)
  auto submitResult = proto.submitRequest(makePostRequest(std::string(70000, 'X')));
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  // Inflate stream window to near INT32_MAX via WINDOW_UPDATE
  feed(proto, makeWindowUpdate(sid, 0x7ffffffe));
  REQUIRE_FALSE(proto.isClosed());

  // INITIAL_WINDOW_SIZE change from 1 to 0x7fffffff: delta = 0x7ffffffe
  // Stream window (~0x7fff0000 after body sent) + delta → overflow
  ByteBuffer settingsUpdate =
      makeServerSettings({{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE),
                           Http2SettingInfo::maximumWindowSize()}});
  auto result = feed(proto, settingsUpdate);
  requireConnectionError(result, ErrorCode::FLOW_CONTROL_ERROR, proto);
}

TEST_CASE("2.3.4 HEADER_TABLE_SIZE change updates HPACK encoder", "[settings]") {
  auto getHeaderSize = [](Http2Protocol& p) -> std::uint32_t {
    auto r = p.submitRequest(makeGetRequest("/same-path"));
    REQUIRE(r.hasSubmittedStream);
    auto frames = decodeOutbound(r);
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::HEADERS) {
        return f.header.payloadLength;
      }
    }
    return 0;
  };

  // With HEADER_TABLE_SIZE=0, second identical request should NOT be
  // significantly smaller (no dynamic table savings).
  Http2Protocol proto0;
  handshake(proto0, {{static_cast<uint16_t>(Http2SettingParameter::HEADER_TABLE_SIZE), 0}});
  auto size0First = getHeaderSize(proto0);
  auto size0Second = getHeaderSize(proto0);
  REQUIRE(size0First > 0);
  // At most 2 bytes difference (table size update instruction in first)
  REQUIRE(size0First - size0Second <= 2);

  // With default HEADER_TABLE_SIZE, second request should be significantly
  // smaller due to dynamic table references.
  Http2Protocol proto1;
  handshake(proto1);
  auto size1First = getHeaderSize(proto1);
  auto size1Second = getHeaderSize(proto1);
  REQUIRE(size1Second < size1First);

  // Without dynamic table, second encoding is larger than with it.
  REQUIRE(size0Second > size1Second);
}

TEST_CASE("2.3.6 Multiple SETTINGS frames", "[settings]") {
  Http2Protocol proto;
  proto.start();

  auto r1 = feed(proto,
                 makeServerSettings(
                     {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 10}}));
  auto r2 = feed(proto,
                 makeServerSettings(
                     {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 20}}));
  auto r3 = feed(proto,
                 makeServerSettings(
                     {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 5}}));

  REQUIRE(countEvents(r1, ProtocolEventType::SETTINGS_RECEIVED) == 1);
  REQUIRE(countEvents(r2, ProtocolEventType::SETTINGS_RECEIVED) == 1);
  REQUIRE(countEvents(r3, ProtocolEventType::SETTINGS_RECEIVED) == 1);

  // All 3 should produce SETTINGS ACK
  auto f1 = decodeOutbound(r1);
  auto f2 = decodeOutbound(r2);
  auto f3 = decodeOutbound(r3);
  int ackCount = 0;
  for (auto* frames : {&f1, &f2, &f3}) {
    for (auto& f : *frames) {
      if (f.header.getType() == FrameType::SETTINGS && f.header.hasAckFlag()) {
        ackCount++;
      }
    }
  }
  REQUIRE(ackCount == 3);
}

TEST_CASE("2.3.7 MAX_FRAME_SIZE at exact minimum (16384)", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings =
      makeServerSettings({{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), 16384}});
  auto result = feed(proto, settings);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));
}

TEST_CASE("2.3.8 MAX_FRAME_SIZE at exact maximum (16777215)", "[settings]") {
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), (1u << 24) - 1}});
  auto result = feed(proto, settings);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));
}

TEST_CASE("2.3.9 Multiple settings in single SETTINGS frame all applied", "[settings]") {
  // RFC 9113 §6.5.3: all values in a single SETTINGS frame must be applied.
  Http2Protocol proto;
  proto.start();

  // Send MAX_FRAME_SIZE=32768 and MAX_CONCURRENT_STREAMS=2 in one frame
  ByteBuffer settings = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), 32768},
       {static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 2},
       {static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 16384}});
  auto result = feed(proto, settings);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));

  REQUIRE(proto.peerMaxFrameSize() == 32768);
  REQUIRE(proto.peerMaxConcurrentStreams() == 2);
  REQUIRE(proto.peerInitialWindowSize() == 16384);

  // Verify MAX_CONCURRENT_STREAMS=2 was applied
  feed(proto, makeSettingsAck());
  auto r1 = proto.submitRequest(makeGetRequest("/1"));
  auto r2 = proto.submitRequest(makeGetRequest("/2"));
  auto r3 = proto.submitRequest(makeGetRequest("/3"));
  REQUIRE(r1.hasSubmittedStream);
  REQUIRE(r2.hasSubmittedStream);
  REQUIRE_FALSE(r3.hasSubmittedStream);  // blocked by MAX_CONCURRENT_STREAMS=2

  // Verify MAX_FRAME_SIZE=32768 was applied: 24000-byte body in single DATA frame
  std::string body(24000, 'x');
  auto r4 = proto.submitRequest(makePostRequest(body));
  REQUIRE_FALSE(r4.hasSubmittedStream);  // blocked at 2 concurrent streams
  proto.cancelStream(r1.submittedStreamId, ErrorCode::CANCEL);
  proto.cancelStream(r2.submittedStreamId, ErrorCode::CANCEL);
  auto r5 = proto.submitRequest(makePostRequest(body));
  REQUIRE(r5.hasSubmittedStream);
  auto frames5 = decodeOutbound(r5);
  int dataFrames = 0;
  for (auto& f : frames5) {
    if (f.header.getType() == FrameType::DATA) {
      REQUIRE(f.header.payloadLength <= 32768);
      dataFrames++;
    }
  }
  REQUIRE(dataFrames == 1);  // 24000 fits in single 32768-max frame
}

TEST_CASE("2.3.10 INITIAL_WINDOW_SIZE reduction blocks pending body", "[settings]") {
  // RFC 9113 §6.9.2: A sender MUST track the negative flow-control window
  // and MUST NOT send new flow-controlled frames until it becomes positive.
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100}});

  auto submitResult = proto.submitRequest(makePostRequest(std::string(200, 'X')));
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  // 100 bytes sent due to stream window
  auto frames = decodeOutbound(submitResult);
  std::uint32_t dataBytes = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes == 100);

  // Now server reduces INITIAL_WINDOW_SIZE to 10. Delta = 10 - 100 = -90.
  // Stream window was 0 (100 used of 100), now becomes 0 + (-90) = -90.
  // No new DATA should be emitted.
  ByteBuffer settingsUpdate =
      makeServerSettings({{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 10}});
  auto r2 = feed(proto, settingsUpdate);
  REQUIRE_FALSE(hasEvent(r2, ProtocolEventType::CONNECTION_ERROR));
  auto frames2 = decodeOutbound(r2);
  std::uint32_t dataBytes2 = 0;
  for (auto& f : frames2) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes2 += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes2 == 0);

  // Now WINDOW_UPDATE for 200 should unblock (window becomes -90 + 200 = 110)
  auto r3 = feed(proto, makeWindowUpdate(sid, 200));
  auto frames3 = decodeOutbound(r3);
  std::uint32_t dataBytes3 = 0;
  for (auto& f : frames3) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes3 += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes3 == 100);  // remaining 100 bytes
}

// =============================================================================
// Section 3: HEADERS [headers]
// =============================================================================

TEST_CASE("3.1.1 HEADERS on stream 0", "[headers]") {
  Http2Protocol proto;
  handshake(proto);

  Hpack enc;
  ByteBuffer hdr = makeResponseHeaders(enc, 0, 200, {}, true);
  auto result = feed(proto, hdr);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("3.1.2 HEADERS for never-opened stream", "[headers]") {
  Http2Protocol proto;
  handshake(proto);
  // next_local_stream_id is 1 and no streams submitted
  // streamId=1 is >= next_local_stream_id
  Hpack enc;
  ByteBuffer hdr = makeResponseHeaders(enc, 1, 200, {}, true);
  auto result = feed(proto, hdr);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("3.1.3 HEADERS for peer-initiated even stream", "[headers]") {
  Http2Protocol proto;
  handshake(proto);

  Hpack enc;
  ByteBuffer hdr = makeResponseHeaders(enc, 2, 200, {}, true);
  auto result = feed(proto, hdr);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("3.1.4 HEADERS for already-closed stream", "[headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  completeResponse(proto, enc, sid);
  REQUIRE(proto.activeStreamCount() == 0);

  // Feed HEADERS on same (now closed) stream — silent discard
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("3.1.5 HEADERS with self-dependent PRIORITY (ignored)", "[headers]") {
  // RFC 9113 §5.3.2: priority signaling is deprecated.
  // Self-dependency in the PRIORITY fields should be silently ignored.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}};
  ByteBuffer encoded = enc.encode(headers);

  HeadersFrame frame(sid);
  frame.header.setPriorityFlag();
  frame.header.setEndHeadersFlag();
  frame.header.setEndStreamFlag();
  frame.streamDependency = sid;  // self-dependent — ignored per §5.3.2
  frame.weight = 16;
  frame.data = ByteBuffer::from(encoded.begin(), encoded.end());
  frame.header.payloadLength = 5 + frame.data.size();
  ByteBuffer raw = frame.toBytes();

  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("3.1.6 HEADERS with PADDED flag, padding > payload", "[headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Build raw HEADERS with PADDED flag where pad_length > remaining payload
  std::vector<HeaderField> headers = {{":status", "200"}};
  ByteBuffer encoded = enc.encode(headers);
  HeadersFrame frame(sid);
  frame.header.setPaddedFlag();
  frame.header.setEndHeadersFlag();
  frame.header.setEndStreamFlag();
  frame.padLength = 200;  // larger than HPACK block
  frame.data = ByteBuffer::from(encoded.begin(), encoded.end());
  frame.padding = ByteBuffer(200u);
  // payload_length = 1 (pad_length) + data.size() + padding.size()
  // But pad_length claims 200 which exceeds the real data portion
  // We force payload_length to be small so padding > remaining
  frame.header.payloadLength = 1 + frame.data.size() + 200;
  // Actually the issue is pad_length > (payload_length - 1).
  // Let's set payload_length small and use MakeRawFrame to forge it.
  // Simpler: use raw frame with PADDED flag, pad_length byte = 0xFF,
  // and only a few bytes of payload.
  std::string badPayload;
  badPayload += static_cast<char>(0xFF);  // pad_length = 255
  badPayload += "XX";                     // only 2 more bytes — way less than 255
  ByteBuffer raw = makeRawFrame(FrameType::HEADERS,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::PADDED) |
                                              static_cast<uint8_t>(FrameFlag::END_HEADERS) |
                                              static_cast<uint8_t>(FrameFlag::END_STREAM)),
                           sid,
                           badPayload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("3.1.7 HEADERS with PADDED flag, zero-length payload", "[headers]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // PADDED flag set but payload_length=0 → missing pad_length byte
  ByteBuffer raw = makeRawFrame(FrameType::HEADERS,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::PADDED) |
                                              static_cast<uint8_t>(FrameFlag::END_HEADERS)),
                           sid,
                           std::string());
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("3.2.1 Simple 200 OK response", "[headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  ByteBuffer hdr = makeResponseHeaders(enc, sid, 200, {}, true);
  auto result = feed(proto, hdr);

  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));

  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getStatusCode() == 200);
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("3.2.2 Response with extra headers", "[headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  ByteBuffer hdr = makeResponseHeaders(
      enc, sid, 200, {{"content-type", "text/plain"}, {"x-custom", "foo"}}, true);
  auto result = feed(proto, hdr);

  auto evt = findEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED);
  auto& resp = evt.response;
  REQUIRE(resp.getStatusCode() == 200);
  REQUIRE(resp.getHeader("content-type") == "text/plain");
  REQUIRE(resp.getHeader("x-custom") == "foo");
}

TEST_CASE("3.2.3 HEADERS without END_STREAM then DATA", "[headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto r1 = feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  REQUIRE(hasEvent(r1, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE_FALSE(hasEvent(r1, ProtocolEventType::RESPONSE_COMPLETED));

  auto r2 = feed(proto, makeDataFrame(sid, "hello", true));
  REQUIRE(hasEvent(r2, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("3.2.4 HEADERS with PRIORITY flag", "[headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}};
  ByteBuffer encoded = enc.encode(headers);

  HeadersFrame frame(sid);
  frame.header.setPriorityFlag();
  frame.header.setEndHeadersFlag();
  frame.header.setEndStreamFlag();
  frame.streamDependency = 0;
  frame.weight = 16;
  frame.data = ByteBuffer::from(encoded.begin(), encoded.end());
  frame.header.payloadLength = 5 + frame.data.size();
  ByteBuffer raw = frame.toBytes();

  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("3.2.5 HEADERS with PADDED + PRIORITY flags combined", "[headers]") {
  // RFC 9113 §6.2: both PADDED and PRIORITY flags can be set simultaneously.
  // Payload: 1 (pad length) + 5 (priority) + HPACK block + padding.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"x-combo", "padded-priority"}};
  ByteBuffer encoded = enc.encode(headers);

  HeadersFrame frame(sid);
  frame.header.setPaddedFlag();
  frame.header.setPriorityFlag();
  frame.header.setEndHeadersFlag();
  frame.header.setEndStreamFlag();
  frame.streamDependency = 0;
  frame.weight = 16;
  frame.padLength = 4;
  frame.data = ByteBuffer::from(encoded.begin(), encoded.end());
  frame.padding = ByteBuffer(4u);
  for (int i = 0; i < 4; ++i) {
    frame.padding.append<uint8_t>(0);
  }
  // payloadLength = 1 (pad_length) + 5 (priority) + data + 4 (padding)
  frame.header.payloadLength = 1 + 5 + frame.data.size() + 4;
  ByteBuffer raw = frame.toBytes();

  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getStatusCode() == 200);
  REQUIRE(completed.response.getHeader("x-combo") == "padded-priority");
}

// =============================================================================
// Section 4: DATA [data]
// =============================================================================

TEST_CASE("4.1.1 DATA on stream 0", "[data]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer data = makeDataFrame(0, "hello", false);
  auto result = feed(proto, data);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("4.1.2 DATA for never-opened stream", "[data]") {
  Http2Protocol proto;
  handshake(proto);

  // streamId=99, never opened
  ByteBuffer data = makeDataFrame(99, "hello", false);
  auto result = feed(proto, data);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("4.1.3 DATA for peer-initiated even stream", "[data]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer data = makeDataFrame(2, "hello", false);
  auto result = feed(proto, data);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("4.1.4 DATA before response headers", "[data]") {
  // RFC 9113 §8.1/§8.1.1: response must start with HEADERS.
  // DATA before HEADERS is malformed → stream error.
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  ByteBuffer data = makeDataFrame(sid, "hello", false);
  auto result = feed(proto, data);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

// Test 4.1.5 removed: subsumed by test 25.25 (exact refund verification)

TEST_CASE("4.1.6 DATA after remote END_STREAM", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  // Body larger than default window (65535) so it won't be fully sent,
  // keeping localEndStreamSent=false. This simulates a large POST upload
  // where the server responds early (e.g. 403) before the body finishes.
  auto submitResult = proto.submitRequest(makePostRequest(std::string(70000, 'X')));
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  // Server responds early with END_STREAM — stream enters HALF_CLOSED_REMOTE
  auto hdrResult = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  // Verify the 200 response was properly delivered before the violation
  REQUIRE(hasEvent(hdrResult, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  auto hdrEvt = findEvent(hdrResult, ProtocolEventType::STREAM_HEADERS_RECEIVED);
  REQUIRE(hdrEvt.response.getStatusCode() == 200);
  // Server violates protocol by sending DATA after its own END_STREAM
  auto result = feed(proto, makeDataFrame(sid, "extra", false));
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::STREAM_CLOSED);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
}

TEST_CASE("4.1.7 DATA with PADDED flag, padding > remaining", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // PADDED DATA where pad_length exceeds remaining payload bytes
  std::string badPayload;
  badPayload += static_cast<char>(0xFF);  // pad_length = 255
  badPayload += "AB";                     // only 2 data bytes → 255 > 2
  ByteBuffer raw =
      makeRawFrame(FrameType::DATA, static_cast<uint8_t>(FrameFlag::PADDED), sid, badPayload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("4.1.8 DATA with PADDED flag, zero-length payload", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // PADDED flag set but payload_length=0 → missing pad_length byte
  ByteBuffer raw =
      makeRawFrame(FrameType::DATA, static_cast<uint8_t>(FrameFlag::PADDED), sid, std::string());
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("4.2.1 Single DATA frame with body", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "hello", true));

  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));

  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.body == "hello");
}

TEST_CASE("4.2.2 Multiple DATA frames", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "aaa", false));
  feed(proto, makeDataFrame(sid, "bbb", false));
  auto result = feed(proto, makeDataFrame(sid, "ccc", true));

  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.body == "aaabbbccc");
}

TEST_CASE("4.2.3 Zero-length DATA frame", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "", false));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("4.2.4 DATA with END_STREAM and empty body", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "", true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.body.empty());
}

TEST_CASE("4.2.5 DATA with PADDED flag", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  // DATA with 5 bytes of actual content and 10 bytes of padding
  auto result = feed(proto, makeDataFrame(sid, "hello", true, 10));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  // Body should be only the actual data, not padding
  REQUIRE(completed.response.body == "hello");
}

TEST_CASE("4.2.6 Large DATA across multiple frames", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // 100KB body split into chunks
  std::string largeBody(static_cast<std::size_t>(100) * 1024, 'x');
  const std::size_t chunkSize = 16384;
  std::size_t offset = 0;
  ProtocolStepResult lastResult;
  while (offset < largeBody.size()) {
    std::size_t len = std::min(chunkSize, largeBody.size() - offset);
    bool end = (offset + len >= largeBody.size());
    lastResult = feed(proto, makeDataFrame(sid, largeBody.substr(offset, len), end));
    offset += len;
  }

  REQUIRE(hasEvent(lastResult, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(lastResult, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.body.size() == largeBody.size());
  REQUIRE(completed.response.body == largeBody);
}

TEST_CASE("4.3.1 Content-Length matches received data", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "5"}}, false));
  auto result = feed(proto, makeDataFrame(sid, "hello", true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("4.3.2 Content-Length mismatch: too few bytes", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "10"}}, false));
  auto result = feed(proto, makeDataFrame(sid, "hello", true));  // 5 < 10
  // Stream error with PROTOCOL_ERROR
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::PROTOCOL_ERROR);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
}

TEST_CASE("4.3.3 Content-Length mismatch: too many bytes", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "3"}}, false));
  auto result = feed(proto, makeDataFrame(sid, "hello", true));  // 5 > 3
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::PROTOCOL_ERROR);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
}

TEST_CASE("4.3.4 Content-Length exempt: HEAD request", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);

  auto submitResult = proto.submitRequest(makeHeadRequest());
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  // HEAD response: content-length: 100 but 0 bytes data
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "100"}}, true));
  // No error — HEAD exempt from Content-Length validation
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("4.3.5 Content-Length exempt: 204 No Content", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // content-length: 100 would fail on a normal 200, but 204 is exempt
  auto result = feed(proto, makeResponseHeaders(enc, sid, 204, {{"content-length", "100"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("4.3.6 Content-Length exempt: 304 Not Modified", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeResponseHeaders(enc, sid, 304, {{"content-length", "100"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("4.3.7 No Content-Length header", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "hello", true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("4.3.8 Duplicate Content-Length with same value", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {
      {":status", "200"}, {"content-length", "5"}, {"content-length", "5"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, false);
  feed(proto, raw);

  auto result = feed(proto, makeDataFrame(sid, "hello", true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("4.3.9 Duplicate Content-Length with conflicting values", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Encode headers with two different content-length values
  std::vector<HeaderField> headers = {
      {":status", "200"}, {"content-length", "5"}, {"content-length", "10"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, false, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("4.3.10 Content-Length > 0 with END_STREAM on HEADERS (zero body)", "[data]") {
  // HEADERS with END_STREAM means zero body bytes received.
  // If Content-Length > 0, this is a mismatch → stream error PROTOCOL_ERROR.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "5"}}, true));
  // receivedContentLength=0 vs expectedContentLength=5 → PROTOCOL_ERROR
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::PROTOCOL_ERROR);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
}

TEST_CASE("4.3.11 Content-Length mismatch across multiple DATA frames", "[data]") {
  // Content-Length=5, send 3 bytes in first DATA then 3 more with END_STREAM.
  // Total=6 != 5 → stream error PROTOCOL_ERROR on END_STREAM.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "5"}}, false));
  // First DATA: 3 bytes, no END_STREAM — still within budget, no error
  auto r1 = feed(proto, makeDataFrame(sid, "abc", false));
  REQUIRE_FALSE(hasEvent(r1, ProtocolEventType::RESPONSE_FAILED));

  // Second DATA: 3 bytes with END_STREAM — total=6 != 5 → error
  auto r2 = feed(proto, makeDataFrame(sid, "def", true));
  auto frames = decodeOutbound(r2);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::PROTOCOL_ERROR);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(r2, ProtocolEventType::RESPONSE_FAILED));
}

TEST_CASE("4.3.12 Content-Length with non-numeric value", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"content-length", "-1"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, false);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("4.3.13 Content-Length with empty value", "[data]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"content-length", ""}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, false);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("4.3.14 Content-Length with leading zeros (impl behavior)", "[data][impl_behavior]") {
  // NOTE: RFC 9110 §8.6 says a sender MUST NOT send Content-Length with
  // leading zeros. The current implementation tolerates "005" → 5.
  // This test documents implementation behavior, not a RFC requirement.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "005"}}, false));
  auto result = feed(proto, makeDataFrame(sid, "hello", true));
  // The parser accepts "005" → expectedContentLength=5, body=5 → match, no error
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

// =============================================================================
// Section 5: RST_STREAM [rst_stream]
// =============================================================================

TEST_CASE("5.1 RST_STREAM on stream 0", "[rst_stream]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer rst = makeRstStream(0, ErrorCode::CANCEL);
  auto result = feed(proto, rst);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("5.2 RST_STREAM payload != 4 (too small and too large)", "[rst_stream]") {
  for (int size : {3, 5}) {
    Http2Protocol proto;
    handshake(proto);

    std::string payload(static_cast<std::size_t>(size), '\0');
    ByteBuffer raw = makeRawFrame(FrameType::RST_STREAM, 0, 1, payload);
    auto result = feed(proto, raw);
    requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
  }
}

TEST_CASE("5.4 RST_STREAM for idle stream", "[rst_stream]") {
  Http2Protocol proto;
  handshake(proto);

  // streamId=99, never opened (>= next_local_stream_id)
  ByteBuffer rst = makeRstStream(99, ErrorCode::CANCEL);
  auto result = feed(proto, rst);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("5.5 RST_STREAM for closed/removed stream", "[rst_stream]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  completeResponse(proto, enc, sid);
  REQUIRE(proto.activeStreamCount() == 0);

  // RST_STREAM on the now-closed stream — silent no-op
  auto result = feed(proto, makeRstStream(sid, ErrorCode::CANCEL));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("5.6 Normal RST_STREAM cancels active stream", "[rst_stream]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  REQUIRE(proto.activeStreamCount() == 1);

  auto result = feed(proto, makeRstStream(sid, ErrorCode::CANCEL));
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_RESET_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_CLOSED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  REQUIRE(proto.activeStreamCount() == 0);

  auto rstEvt = findEvent(result, ProtocolEventType::STREAM_RESET_RECEIVED);
  REQUIRE(rstEvt.errorCode == ErrorCode::CANCEL);
}

TEST_CASE("5.7 RST_STREAM with various error codes", "[rst_stream]") {
  for (auto code :
       {ErrorCode::INTERNAL_ERROR, ErrorCode::REFUSED_STREAM, ErrorCode::FLOW_CONTROL_ERROR}) {
    Http2Protocol proto;
    handshake(proto);
    StreamId sid = submitSimpleGet(proto);

    auto result = feed(proto, makeRstStream(sid, code));
    auto rstEvt = findEvent(result, ProtocolEventType::STREAM_RESET_RECEIVED);
    REQUIRE(rstEvt.errorCode == code);
  }
}

TEST_CASE("5.8 RST_STREAM during CONTINUATION sequence", "[rst_stream]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // HEADERS without END_HEADERS
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false, false));

  // Now expect CONTINUATION, but feed RST_STREAM instead
  auto result = feed(proto, makeRstStream(sid, ErrorCode::CANCEL));
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

// =============================================================================
// Section 6: PING [ping]
// =============================================================================

TEST_CASE("6.1 PING echoed with ACK", "[ping]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = feed(proto, makePing("12345678"));

  auto frames = decodeOutbound(result);
  bool foundAck = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::PING && f.header.hasAckFlag()) {
      PingFrame pf(f);
      std::string payload(pf.data.begin(), pf.data.end());
      REQUIRE(payload == "12345678");
      foundAck = true;
    }
  }
  REQUIRE(foundAck);
  REQUIRE(hasEvent(result, ProtocolEventType::PING_RECEIVED));
}

TEST_CASE("6.2 PING ACK does not trigger reply", "[ping]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = feed(proto, makePing("12345678", true));
  REQUIRE(hasEvent(result, ProtocolEventType::PING_ACK_RECEIVED));

  auto frames = decodeOutbound(result);
  for (auto& f : frames) {
    REQUIRE_FALSE(f.header.getType() == FrameType::PING);
  }
}

TEST_CASE("6.3 PING opaque data preserved", "[ping]") {
  Http2Protocol proto;
  handshake(proto);

  std::string opaque(8, '\xFF');
  auto result = feed(proto, makePing(opaque));

  auto frames = decodeOutbound(result);
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::PING && f.header.hasAckFlag()) {
      PingFrame pf(f);
      std::string payload(pf.data.begin(), pf.data.end());
      REQUIRE(payload == opaque);
    }
  }
}

TEST_CASE("6.4 PING on non-zero stream", "[ping]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer raw = makeRawFrame(FrameType::PING, 0, 1, std::string(8, '\0'));
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("6.5 PING payload != 8 (too short and too long)", "[ping]") {
  for (int size : {7, 9}) {
    Http2Protocol proto;
    handshake(proto);

    ByteBuffer raw =
        makeRawFrame(FrameType::PING, 0, 0, std::string(static_cast<std::size_t>(size), '\0'));
    auto result = feed(proto, raw);
    requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
  }
}

TEST_CASE("6.7 Multiple PINGs with correct opaque data echo", "[ping]") {
  Http2Protocol proto;
  handshake(proto);

  const char* payloads[] = {"aaaaaaaa", "bbbbbbbb", "cccccccc"};
  for (const char* payload : payloads) {
    auto result = feed(proto, makePing(payload));
    auto frames = decodeOutbound(result);
    bool foundAck = false;
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::PING && f.header.hasAckFlag()) {
        PingFrame pf(f);
        std::string ackPayload(pf.data.begin(), pf.data.end());
        REQUIRE(ackPayload == payload);
        foundAck = true;
      }
    }
    REQUIRE(foundAck);
  }
}

// =============================================================================
// Section 7: GOAWAY [goaway]
// =============================================================================

TEST_CASE("7.1 GOAWAY on non-zero stream", "[goaway]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer raw = makeRawFrame(FrameType::GOAWAY, 0, 1, std::string(8, '\0'));
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("7.2 GOAWAY payload < 8 bytes", "[goaway]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer raw = makeRawFrame(FrameType::GOAWAY, 0, 0, std::string(4, '\0'));
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

TEST_CASE("7.3 GOAWAY fails streams beyond lastStreamId", "[goaway]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");
  REQUIRE(proto.activeStreamCount() == 3);

  // GOAWAY with lastStreamId=3: stream 5 should fail
  auto result = feed(proto, makeGoAway(s3, ErrorCode::NO_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::GOAWAY_RECEIVED));

  auto fails = findEvents(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails.size() == 1);
  REQUIRE(fails[0].streamId == s5);
  REQUIRE(proto.activeStreamCount() == 2);  // s1 and s3 remain
  (void) s1;
}

TEST_CASE("7.4 GOAWAY with NO_ERROR and lastStreamId=0", "[goaway]") {
  Http2Protocol proto;
  handshake(proto);
  submitSimpleGet(proto, "/1");
  submitSimpleGet(proto, "/2");

  auto result = feed(proto, makeGoAway(0, ErrorCode::NO_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::GOAWAY_RECEIVED));

  // All streams should be failed
  auto fails = findEvents(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails.size() == 2);
}

TEST_CASE("7.5 No new requests after GOAWAY", "[goaway]") {
  Http2Protocol proto;
  handshake(proto);

  feed(proto, makeGoAway(0, ErrorCode::NO_ERROR));
  REQUIRE_FALSE(proto.canSubmitRequests());

  auto result = proto.submitRequest(makeGetRequest());
  REQUIRE_FALSE(result.hasSubmittedStream);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  auto fail = findEvent(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fail.errorCode == ErrorCode::REFUSED_STREAM);
}

TEST_CASE("7.6 GOAWAY with debug data", "[goaway]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = feed(proto, makeGoAway(0, ErrorCode::NO_ERROR, "going away"));
  auto evt = findEvent(result, ProtocolEventType::GOAWAY_RECEIVED);
  REQUIRE(evt.debugData == "going away");
}

TEST_CASE("7.7 GOAWAY preserves streams <= lastStreamId", "[goaway]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  submitSimpleGet(proto, "/2");

  feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));

  // Stream s1 can still complete normally
  auto result = feed(proto, makeResponseHeaders(enc, s1, 200, {}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("7.8 Multiple GOAWAYs", "[goaway]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");
  REQUIRE(proto.activeStreamCount() == 3);

  auto r1 = feed(proto, makeGoAway(s5, ErrorCode::NO_ERROR));
  REQUIRE(proto.activeStreamCount() == 3);

  auto r2 = feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));
  // Should fail s3 and s5
  auto fails = findEvents(r2, ProtocolEventType::RESPONSE_FAILED);
  // s5 is still in the map after the first GOAWAY since lastStreamId was 5
  // Second GOAWAY with last=1 should fail s3 and s5
  REQUIRE(fails.size() == 2);
  REQUIRE(proto.activeStreamCount() == 1);  // only s1 remains
  (void) s1;
  (void) s3;
}

TEST_CASE("7.9 Two-phase graceful shutdown", "[goaway]") {
  // RFC 9113 §6.8: server sends initial GOAWAY with lastStreamId=2^31-1,
  // then a second GOAWAY with the actual last stream ID.
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Phase 1: GOAWAY with max stream ID — no streams should be failed
  auto r1 = feed(proto, makeGoAway(0x7fffffffu, ErrorCode::NO_ERROR));
  REQUIRE(hasEvent(r1, ProtocolEventType::GOAWAY_RECEIVED));
  auto fails1 = findEvents(r1, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails1.empty());
  REQUIRE(proto.activeStreamCount() == 2);
  // New requests blocked
  REQUIRE_FALSE(proto.canSubmitRequests());

  // Phase 2: GOAWAY with actual last stream ID
  auto r2 = feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));
  auto fails2 = findEvents(r2, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails2.size() == 1);
  REQUIRE(fails2[0].streamId == s3);
  REQUIRE(proto.activeStreamCount() == 1);
}

TEST_CASE("7.10 GOAWAY preserves streams for full HEADERS+DATA+END_STREAM lifecycle", "[goaway]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // GOAWAY with lastStreamId=s1: s3 is failed, s1 survives
  feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));
  REQUIRE(proto.activeStreamCount() == 1);

  // s1 can still receive full response lifecycle
  auto r1 = feed(proto, makeResponseHeaders(enc, s1, 200, {{"x-ok", "true"}}, false));
  REQUIRE(hasEvent(r1, ProtocolEventType::STREAM_HEADERS_RECEIVED));

  feed(proto, makeDataFrame(s1, "body-data", false));

  auto r3 = feed(proto, makeDataFrame(s1, "", true));
  REQUIRE(hasEvent(r3, ProtocolEventType::RESPONSE_COMPLETED));
  REQUIRE(proto.activeStreamCount() == 0);
  (void) s3;
}

TEST_CASE("7.11 GOAWAY error code propagates to failed streams", "[goaway]") {
  // When GOAWAY carries a non-NO_ERROR code, failed streams should report
  // that error code in their RESPONSE_FAILED events.
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");

  // GOAWAY with INTERNAL_ERROR, lastStreamId = s1
  auto result = feed(proto, makeGoAway(s1, ErrorCode::INTERNAL_ERROR, "server overloaded"));
  REQUIRE(hasEvent(result, ProtocolEventType::GOAWAY_RECEIVED));
  auto goawayEvt = findEvent(result, ProtocolEventType::GOAWAY_RECEIVED);
  REQUIRE(goawayEvt.errorCode == ErrorCode::INTERNAL_ERROR);
  REQUIRE(goawayEvt.debugData == "server overloaded");

  // s3 and s5 should be failed with INTERNAL_ERROR (not NO_ERROR)
  auto fails = findEvents(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails.size() == 2);
  for (auto& f : fails) {
    REQUIRE(f.errorCode == ErrorCode::INTERNAL_ERROR);
  }
  REQUIRE(proto.activeStreamCount() == 1);
  (void) s1;
  (void) s3;
  (void) s5;
}

TEST_CASE("7.12 GOAWAY with increasing lastStreamId: state does not regress", "[goaway]") {
  // RFC 9113 §6.8: "Endpoints MUST NOT increase the value they send in
  // the last stream identifier." This is a sender constraint; as receiver
  // we must not resurrect failed streams or regress our goaway state.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  submitSimpleGet(proto, "/2");  // s3
  submitSimpleGet(proto, "/3");  // s5
  REQUIRE(proto.activeStreamCount() == 3);

  // First GOAWAY(last=s1) → s3 and s5 are failed
  auto r1 = feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));
  auto fails1 = findEvents(r1, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails1.size() == 2);
  REQUIRE(proto.activeStreamCount() == 1);

  // Second GOAWAY with higher lastStreamId (server RFC violation)
  auto r2 = feed(proto, makeGoAway(s1 + 4, ErrorCode::NO_ERROR));
  // No new RESPONSE_FAILED — s3/s5 are already gone, cannot be resurrected
  REQUIRE(findEvents(r2, ProtocolEventType::RESPONSE_FAILED).empty());
  // s1 must still be alive and serviceable
  REQUIRE(proto.activeStreamCount() == 1);
  // No new requests allowed — receivedGoaway is still set
  auto r3 = proto.submitRequest(makeGetRequest("/new"));
  REQUIRE_FALSE(r3.hasSubmittedStream);
  // s1 can still complete normally
  auto hdrResult = feed(proto, makeResponseHeaders(enc, s1, 200, {{"x-ok", "true"}}, true));
  REQUIRE(hasEvent(hdrResult, ProtocolEventType::RESPONSE_COMPLETED));
  REQUIRE(proto.activeStreamCount() == 0);
}

// =============================================================================
// Section 8: WINDOW_UPDATE [window_update]
// =============================================================================

TEST_CASE("8.1.1 WINDOW_UPDATE payload != 4 (too small and too large)", "[window_update]") {
  for (int size : {3, 5}) {
    Http2Protocol proto;
    handshake(proto);

    ByteBuffer raw = makeRawFrame(
        FrameType::WINDOW_UPDATE, 0, 0, std::string(static_cast<std::size_t>(size), '\0'));
    auto result = feed(proto, raw);
    requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
  }
}

TEST_CASE("8.1.3 Connection WINDOW_UPDATE increment=0", "[window_update]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = feed(proto, makeWindowUpdate(0, 0));
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("8.1.4 Stream WINDOW_UPDATE increment=0", "[window_update]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeWindowUpdate(sid, 0));
  // Stream error: RST_STREAM with PROTOCOL_ERROR
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::PROTOCOL_ERROR);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
}

TEST_CASE("8.1.5 Connection window overflow", "[window_update]") {
  Http2Protocol proto;
  handshake(proto);

  // The default connection outflow is 65535. If we add 2^31-1 it will overflow.
  auto result = feed(proto, makeWindowUpdate(0, Http2SettingInfo::maximumWindowSize()));
  requireConnectionError(result, ErrorCode::FLOW_CONTROL_ERROR, proto);
}

TEST_CASE("8.1.6 Stream window overflow", "[window_update]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Stream outflow default 65535, increment by max → overflow
  auto result = feed(proto, makeWindowUpdate(sid, Http2SettingInfo::maximumWindowSize()));
  // Stream error with FLOW_CONTROL_ERROR
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::FLOW_CONTROL_ERROR);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
}

TEST_CASE("8.1.7 WINDOW_UPDATE for idle stream", "[window_update]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = feed(proto, makeWindowUpdate(99, 1));
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("8.1.8 WINDOW_UPDATE for closed/removed stream", "[window_update]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  completeResponse(proto, enc, sid);
  REQUIRE(proto.activeStreamCount() == 0);

  auto result = feed(proto, makeWindowUpdate(sid, 1));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::WINDOW_UPDATE_RECEIVED));
}

TEST_CASE("8.2.1 Connection WINDOW_UPDATE unblocks pending body", "[window_update]") {
  Http2Protocol proto;
  // Set large stream window so connection window (65535) is the bottleneck.
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100000}});

  // Submit body of 70000 bytes — exceeds connection window (65535)
  std::string body(70000, 'X');
  auto submitResult = proto.submitRequest(makePostRequest(body));
  REQUIRE(submitResult.hasSubmittedStream);

  // Only 65535 bytes should be sent (limited by connection window, not stream)
  auto frames = decodeOutbound(submitResult);
  std::uint32_t dataBytes = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes == 65535);

  // Connection WINDOW_UPDATE on stream 0 should unblock remaining body
  auto result = feed(proto, makeWindowUpdate(0, 10000));
  auto frames2 = decodeOutbound(result);
  std::uint32_t dataBytes2 = 0;
  for (auto& f : frames2) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes2 += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes2 == 70000 - 65535);  // remaining 4465 bytes
}

TEST_CASE("8.2.2 Stream WINDOW_UPDATE unblocks pending body", "[window_update]") {
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 5}});

  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  StreamId sid = submitResult.submittedStreamId;

  // 5 bytes sent. Now WINDOW_UPDATE for stream only.
  auto result = feed(proto, makeWindowUpdate(sid, 100));
  auto frames = decodeOutbound(result);
  std::uint32_t dataBytes = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes == 5);  // remaining 5 bytes
}

TEST_CASE("8.2.3 Received DATA triggers WINDOW_UPDATE", "[window_update]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // Feed a large enough DATA to trigger window update
  // inflowMinRefresh = 4 << 10 = 4096
  std::string bigData(5000, 'x');
  auto result = feed(proto, makeDataFrame(sid, bigData, false));

  auto frames = decodeOutbound(result);
  bool foundConnWu = false;
  bool foundStreamWu = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::WINDOW_UPDATE) {
      if (f.header.streamId == 0) {
        foundConnWu = true;
      }
      if (f.header.streamId == sid) {
        foundStreamWu = true;
      }
    }
  }
  REQUIRE(foundConnWu);
  REQUIRE(foundStreamWu);
}

TEST_CASE("8.2.4 Reserved bit masked in stream increment", "[window_update]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Create WINDOW_UPDATE with bit 31 set (reserved bit)
  WindowUpdateFrame frame(sid);
  frame.windowSizeIncrement = 0x80000001u;  // reserved bit + 1
  frame.header.payloadLength = 4;
  ByteBuffer raw = frame.toBytes();

  auto result = feed(proto, raw);
  // The reserved bit should be masked off, effective increment = 1
  REQUIRE(hasEvent(result, ProtocolEventType::WINDOW_UPDATE_RECEIVED));
  auto evt = findEvent(result, ProtocolEventType::WINDOW_UPDATE_RECEIVED);
  REQUIRE(evt.windowSizeIncrement == 1);
}

TEST_CASE("8.2.5 Reserved bit masked in connection increment", "[window_update]") {
  Http2Protocol proto;
  handshake(proto);

  // Connection-level WINDOW_UPDATE with bit 31 set (reserved bit)
  WindowUpdateFrame frame(0);
  frame.windowSizeIncrement = 0x80000001u;  // reserved bit + 1
  frame.header.payloadLength = 4;
  ByteBuffer raw = frame.toBytes();

  auto result = feed(proto, raw);
  // The reserved bit should be masked off, effective increment = 1
  // Should NOT trigger flow control error (increment is just 1)
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::WINDOW_UPDATE_RECEIVED));
  auto evt = findEvent(result, ProtocolEventType::WINDOW_UPDATE_RECEIVED);
  REQUIRE(evt.windowSizeIncrement == 1);
}

// =============================================================================
// Section 9: CONTINUATION [continuation]
// =============================================================================

TEST_CASE("9.1 CONTINUATION on stream 0", "[continuation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Set up continuation expectation
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false, false));

  ByteBuffer emptyBlock = ByteBuffer(1u);
  emptyBlock.append<uint8_t>(0);
  ByteBuffer cont = makeContinuation(0, emptyBlock, true);
  auto result = feed(proto, cont);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("9.2 CONTINUATION without preceding HEADERS", "[continuation]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer emptyBlock = ByteBuffer(1u);
  emptyBlock.append<uint8_t>(0);
  ByteBuffer cont = makeContinuation(1, emptyBlock, true);
  auto result = feed(proto, cont);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("9.3 CONTINUATION for wrong stream", "[continuation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  submitSimpleGet(proto, "/2");

  // HEADERS on sid without END_HEADERS
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false, false));

  // CONTINUATION on wrong stream
  ByteBuffer emptyBlock = ByteBuffer(1u);
  emptyBlock.append<uint8_t>(0);
  ByteBuffer cont = makeContinuation(sid + 2, emptyBlock, true);
  auto result = feed(proto, cont);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("9.4 Normal HEADERS + CONTINUATION assembly", "[continuation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Encode headers
  std::vector<HeaderField> headers = {{":status", "200"}, {"x-test", "value"}};
  ByteBuffer encoded = enc.encode(headers);

  // Split into two parts
  std::uint32_t half = encoded.size() / 2;
  ByteBuffer part1 = ByteBuffer::from(encoded.begin(), encoded.begin() + half);
  ByteBuffer part2 = ByteBuffer::from(encoded.begin() + half, encoded.end());

  // HEADERS without END_HEADERS
  ByteBuffer hdr = makeResponseHeadersRaw(part1, sid, true, false);
  feed(proto, hdr);

  // CONTINUATION with END_HEADERS
  ByteBuffer cont = makeContinuation(sid, part2, true);
  auto result = feed(proto, cont);
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("9.5 Multiple CONTINUATION frames", "[continuation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Encode headers
  std::vector<HeaderField> headers = {
      {":status", "200"}, {"x-a", "aaa"}, {"x-b", "bbb"}, {"x-c", "ccc"}};
  ByteBuffer encoded = enc.encode(headers);

  // Split into 4 parts
  std::uint32_t total = encoded.size();
  std::uint32_t q = total / 4;
  ByteBuffer p1 = ByteBuffer::from(encoded.begin(), encoded.begin() + q);
  ByteBuffer p2 = ByteBuffer::from(encoded.begin() + q, encoded.begin() + static_cast<std::size_t>(2 * q));
  ByteBuffer p3 = ByteBuffer::from(encoded.begin() + static_cast<std::size_t>(2 * q),
                           encoded.begin() + static_cast<std::size_t>(3 * q));
  ByteBuffer p4 = ByteBuffer::from(encoded.begin() + static_cast<std::size_t>(3 * q), encoded.end());

  feed(proto, makeResponseHeadersRaw(p1, sid, true, false));
  feed(proto, makeContinuation(sid, p2, false));
  feed(proto, makeContinuation(sid, p3, false));
  auto result = feed(proto, makeContinuation(sid, p4, true));
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("9.6 Non-CONTINUATION frame during header block", "[continuation]") {
  // RFC 9113 §4.3: any frame type other than CONTINUATION during a header
  // block is a connection error PROTOCOL_ERROR.
  struct Case {
    const char* label;
    std::function<ByteBuffer(StreamId)> makeFrame;
  };
  Case cases[] = {
      {"DATA", [](StreamId sid) { return makeDataFrame(sid, "x", false); }},
      {"PING", [](StreamId) { return makePing("12345678"); }},
      {"SETTINGS",
       [](StreamId) {
         return makeServerSettings(
             {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 50}});
       }},
      {"WINDOW_UPDATE", [](StreamId) { return makeWindowUpdate(0, 1000); }},
      {"GOAWAY", [](StreamId) { return makeGoAway(0, ErrorCode::NO_ERROR); }},
      {"PRIORITY", [](StreamId sid) { return makePriority(sid, 0, 16); }},
  };
  for (auto& c : cases) {
    Http2Protocol proto;
    Hpack enc;
    handshake(proto);
    StreamId sid = submitSimpleGet(proto);
    feed(proto, makeResponseHeaders(enc, sid, 200, {}, false, false));

    auto result = feed(proto, c.makeFrame(sid));
    requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
  }
}

// Test 9.8 removed: used two independent enc.encode() calls, so CONTINUATION
// carried a standalone valid HPACK block, masking the pendingHeaderBlock bug.
// Superseded by test 9.8.1 which uses a single encode split in half.

TEST_CASE("9.8 cancelStream mid-CONTINUATION preserves pendingHeaderBlock", "[continuation]") {
  // When cancelStream is called while expecting_continuation_ is true,
  // the pending header block from the first HEADERS must be preserved
  // (transferred to ghost_header_block_) so the subsequent CONTINUATION
  // can be decoded correctly, keeping HPACK state in sync.
  //
  // This test uses ONE encode call split across HEADERS + CONTINUATION.
  // part2 alone is NOT a valid HPACK block, so if pendingHeaderBlock is
  // lost during cancelStream, we'd get COMPRESSION_ERROR.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Encode a single HPACK block with dynamic-table-updating headers
  std::vector<HeaderField> headers = {{":status", "200"},
                                      {"x-split", "this-header-updates-dynamic-table"}};
  ByteBuffer encoded = enc.encode(headers);

  // Split into two halves
  std::uint32_t half = encoded.size() / 2;
  ByteBuffer part1 = ByteBuffer::from(encoded.begin(), encoded.begin() + half);
  ByteBuffer part2 = ByteBuffer::from(encoded.begin() + half, encoded.end());

  // HEADERS with first half (no END_HEADERS) → pendingHeaderBlock accumulates
  feed(proto, makeResponseHeadersRaw(part1, s1, false, false));

  // Cancel the stream mid-CONTINUATION sequence
  proto.cancelStream(s1, ErrorCode::CANCEL);

  // CONTINUATION with second half — must decode correctly using preserved part1
  auto contResult = feed(proto, makeContinuation(s1, part2, true));
  REQUIRE_FALSE(proto.isClosed());

  // Verify HPACK state is intact — s3 must decode correctly
  auto r3 = feed(proto, makeResponseHeaders(enc, s3, 200, {{"x-after", "ok"}}, true));
  REQUIRE(hasEvent(r3, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(r3, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getHeader("x-after") == "ok");
}

TEST_CASE("9.9 CONTINUATION header block exceeds max size", "[impl_behavior]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Start HEADERS without END_HEADERS, then accumulate many
  // CONTINUATION frames to push the pending_header_block_ past
  // max_header_list_size_ (10MB default).
  // We build a minimal HEADERS first.
  std::vector<HeaderField> initial = {{":status", "200"}};
  ByteBuffer initialHpack = enc.encode(initial);
  feed(proto, makeResponseHeadersRaw(initialHpack, sid, false, false));

  // Feed large CONTINUATION frames. Each can be up to max_frame_size (16384).
  // Need ~10MB / 16384 ≈ 625 frames. That's a lot but feasible.
  // Actually let's check — the check is on pending_header_block_.size().
  // 10MB = 10 * 1024 * 1024 = 10485760. At 16384 per frame ≈ 640 frames.
  // Create one large payload per continuation.
  std::string bigChunk(16384, 'X');
  ByteBuffer big = ByteBuffer::from(bigChunk.begin(), bigChunk.end());
  bool gotError = false;
  for (int i = 0; i < 700; ++i) {
    auto result = feed(proto, makeContinuation(sid, big, false));
    if (hasEvent(result, ProtocolEventType::CONNECTION_ERROR)) {
      auto err = findEvent(result, ProtocolEventType::CONNECTION_ERROR);
      REQUIRE(err.errorCode == ErrorCode::ENHANCE_YOUR_CALM);
      gotError = true;
      break;
    }
  }
  REQUIRE(gotError);
}

TEST_CASE("9.10 Unknown frame type during CONTINUATION sequence", "[continuation]") {
  // RFC 9113 §5.5: extension frames in the middle of a field block
  // MUST be treated as a connection error of type PROTOCOL_ERROR.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // HEADERS without END_HEADERS
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false, false));

  // Feed unknown frame type (0xFE) instead of CONTINUATION
  ByteBuffer raw = makeRawFrame(static_cast<FrameType>(0xFE), 0, sid, std::string("test"));
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("9.11 Ghost CONTINUATION exceeding max header list size", "[impl_behavior]") {
  // RFC 9113 §4.3: even for ghost streams (already closed), the protocol must
  // enforce max_header_list_size on the accumulating header block.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Complete s1 — it's now removed from stream map
  completeResponse(proto, enc, s1);
  REQUIRE(proto.activeStreamCount() == 1);

  // Send HEADERS without END_HEADERS on the closed s1 → ghost path
  std::vector<HeaderField> headers = {{":status", "200"}};
  ByteBuffer initialHpack = enc.encode(headers);
  feed(proto, makeResponseHeadersRaw(initialHpack, s1, false, false));
  REQUIRE_FALSE(proto.isClosed());

  // Accumulate large CONTINUATION frames on ghost stream until exceeding
  // max_header_list_size (10MB default). Each frame is 16384 bytes.
  std::string bigChunk(16384, 'X');
  ByteBuffer big = ByteBuffer::from(bigChunk.begin(), bigChunk.end());
  bool gotError = false;
  for (int i = 0; i < 700; ++i) {
    auto result = feed(proto, makeContinuation(s1, big, false));
    if (hasEvent(result, ProtocolEventType::CONNECTION_ERROR)) {
      auto err = findEvent(result, ProtocolEventType::CONNECTION_ERROR);
      REQUIRE(err.errorCode == ErrorCode::ENHANCE_YOUR_CALM);
      gotError = true;
      break;
    }
  }
  REQUIRE(gotError);
  (void) s3;
}

// =============================================================================
// Section 10: PUSH_PROMISE [push_promise]
// =============================================================================

// Tests 10.1-10.4 removed: push is disabled, so all PUSH_PROMISE frames
// trigger the same "server push disabled" connection error before reaching
// the specific validation logic each test intended to cover.

TEST_CASE("10.1 Any PUSH_PROMISE triggers connection error (push disabled)", "[push_promise]") {
  // Client sends ENABLE_PUSH=0 in initial SETTINGS. Any PUSH_PROMISE
  // from the server MUST be treated as a connection error PROTOCOL_ERROR.
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  ByteBuffer emptyHpack = ByteBuffer(1u);
  emptyHpack.append<uint8_t>(0);
  ByteBuffer raw = makePushPromise(sid, 2, emptyHpack);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("10.2 PUSH_PROMISE with PADDED, padding exceeds payload", "[push_promise]") {
  // Padding validation runs before the push-disabled check.
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::string badPayload;
  badPayload += static_cast<char>(0xFF);  // pad_length = 255
  badPayload += '\0';
  badPayload += '\0';
  badPayload += '\0';
  badPayload += '\x02';  // promisedStreamId = 2
  badPayload += "X";     // 1 byte header block — padding 255 > remaining
  ByteBuffer raw = makeRawFrame(FrameType::PUSH_PROMISE,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::PADDED) |
                                              static_cast<uint8_t>(FrameFlag::END_HEADERS)),
                           sid,
                           badPayload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

// =============================================================================
// Section 11: PRIORITY [priority]
// =============================================================================

TEST_CASE("11.1 PRIORITY on stream 0", "[priority]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer pri = makePriority(0, 1, 16);
  auto result = feed(proto, pri);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

TEST_CASE("11.2 PRIORITY payload != 5 bytes (too small and too large)", "[priority]") {
  // RFC 9113 §6.3: stream error of type FRAME_SIZE_ERROR
  for (int size : {4, 6}) {
    Http2Protocol proto;
    handshake(proto);

    std::string payload(static_cast<std::size_t>(size), '\0');
    ByteBuffer raw = makeRawFrame(FrameType::PRIORITY, 0, 1, payload);
    auto result = feed(proto, raw);
    auto frames = decodeOutbound(result);
    bool foundRst = false;
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::RST_STREAM) {
        RstStreamFrame rst(f);
        REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::FRAME_SIZE_ERROR);
        foundRst = true;
      }
    }
    REQUIRE(foundRst);
    REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));

    // Follow with valid PING to prove parser alignment after wrong-size PRIORITY
    auto pingResult = feed(proto, makePing("PRISIZE!"));
    REQUIRE(hasEvent(pingResult, ProtocolEventType::PING_RECEIVED));
  }
}

TEST_CASE("11.3 PRIORITY self-dependency silently ignored, parser aligned", "[priority]") {
  // RFC 9113 §5.3.2: priority signaling is deprecated.
  // Self-dependency should be silently ignored, not flagged as an error.
  // Verify parser consumes exactly 5 bytes by following with a valid PING.
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  ByteBuffer pri = makePriority(sid, sid, 16);
  auto result = feed(proto, pri);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(result.events.empty());
  REQUIRE(proto.activeStreamCount() == 1);

  // Follow-up PING must be correctly parsed — proves parser byte alignment
  auto pingResult = feed(proto, makePing("ALIGNCHK"));
  REQUIRE(hasEvent(pingResult, ProtocolEventType::PING_RECEIVED));
}

TEST_CASE("11.4 PRIORITY self-dependency on closed stream silently ignored", "[priority]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  completeResponse(proto, enc, sid);
  REQUIRE(proto.activeStreamCount() == 0);

  ByteBuffer pri = makePriority(sid, sid, 16);
  auto result = feed(proto, pri);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(result.events.empty());
}

TEST_CASE("11.5 Normal PRIORITY ignored, parser aligned", "[priority]") {
  // Verify parser consumes exactly 5 bytes and subsequent frames decode correctly.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  ByteBuffer pri = makePriority(sid, 0, 16);
  auto result = feed(proto, pri);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(result.events.empty());

  // Follow with a valid HEADERS response — must decode correctly
  auto hdrResult = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-after-pri", "ok"}}, true));
  REQUIRE(hasEvent(hdrResult, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(hdrResult, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getHeader("x-after-pri") == "ok");
}

// =============================================================================
// Section 12: Request Submission [submit]
// =============================================================================

TEST_CASE("12.1.1 Submit before Start", "[submit]") {
  Http2Protocol proto;
  auto result = proto.submitRequest(makeGetRequest());
  REQUIRE_FALSE(result.hasSubmittedStream);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  auto fail = findEvent(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fail.errorCode == ErrorCode::REFUSED_STREAM);
}

// Test 12.1.2 removed: strict subset of test 12.1.3 (close() sends GOAWAY)

TEST_CASE("12.1.3 Submit after sent GOAWAY", "[submit]") {
  Http2Protocol proto;
  handshake(proto);
  proto.close();  // sends GOAWAY

  auto result = proto.submitRequest(makeGetRequest());
  REQUIRE_FALSE(result.hasSubmittedStream);
  auto fail = findEvent(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fail.errorCode == ErrorCode::REFUSED_STREAM);
}

TEST_CASE("12.1.4 Submit after received GOAWAY", "[submit]") {
  Http2Protocol proto;
  handshake(proto);
  feed(proto, makeGoAway(0, ErrorCode::NO_ERROR));

  auto result = proto.submitRequest(makeGetRequest());
  REQUIRE_FALSE(result.hasSubmittedStream);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
}

TEST_CASE("12.1.5 Invalid request: missing :method", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  Request req;
  req.headers[":scheme"] = "https";
  req.headers[":path"] = "/";
  auto result = proto.submitRequest(req);
  REQUIRE_FALSE(result.hasSubmittedStream);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  auto fail = findEvent(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fail.errorCode == ErrorCode::PROTOCOL_ERROR);
}

TEST_CASE("12.1.6 Invalid request: missing :scheme", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  Request req;
  req.headers[":method"] = "GET";
  req.headers[":path"] = "/";
  auto result = proto.submitRequest(req);
  REQUIRE_FALSE(result.hasSubmittedStream);
  // RFC 9113 §8.3.1: non-CONNECT requests MUST include :scheme
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  auto fail = findEvent(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fail.errorCode == ErrorCode::PROTOCOL_ERROR);
}

TEST_CASE("12.1.7 Invalid request: missing :path", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  Request req;
  req.headers[":method"] = "GET";
  req.headers[":scheme"] = "https";
  auto result = proto.submitRequest(req);
  REQUIRE_FALSE(result.hasSubmittedStream);
  // RFC 9113 §8.3.1: non-CONNECT requests MUST include :path
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  auto fail = findEvent(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fail.errorCode == ErrorCode::PROTOCOL_ERROR);
}

TEST_CASE("12.1.8 Valid CONNECT request", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  Request req;
  req.headers[":method"] = "CONNECT";
  req.headers[":authority"] = "example.com:443";
  auto result = proto.submitRequest(req);
  REQUIRE(result.hasSubmittedStream);
}

// Test 12.1.9 removed: duplicate of test 2.3.1 (MAX_CONCURRENT_STREAMS)

TEST_CASE("12.1.10 Stream IDs progress correctly and are never reused", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  // Stream IDs go 1, 3, 5, ... (odd, monotonically increasing, never reused).
  // NOTE: true stream ID exhaustion (nextStreamId > 2^31-1) cannot be tested
  // without exposing a setter for nextStreamId — verifying progression only.
  std::vector<StreamId> usedIds;
  for (int i = 0; i < 10; ++i) {
    auto r = proto.submitRequest(makeGetRequest("/" + std::to_string(i)));
    REQUIRE(r.hasSubmittedStream);
    usedIds.push_back(r.submittedStreamId);
    proto.cancelStream(r.submittedStreamId, ErrorCode::CANCEL);
  }
  // Verify odd, monotonically increasing, never reused
  for (std::size_t i = 0; i < usedIds.size(); ++i) {
    REQUIRE((usedIds[i] & 1u) == 1u);  // odd
    if (i > 0) {
      REQUIRE(usedIds[i] > usedIds[i - 1]);       // increasing
      REQUIRE(usedIds[i] == usedIds[i - 1] + 2);  // step of 2
    }
  }
  REQUIRE(usedIds[0] == 1);
  REQUIRE(proto.nextLocalStreamId() == 21);
}

TEST_CASE("12.2.1 GET request emits HEADERS with END_STREAM", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.submitRequest(makeGetRequest());
  auto frames = decodeOutbound(result);

  // Find the HEADERS frame
  bool foundHeaders = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::HEADERS) {
      REQUIRE(f.header.hasEndStreamFlag());
      REQUIRE(f.header.hasEndHeadersFlag());
      foundHeaders = true;
    }
  }
  REQUIRE(foundHeaders);
}

TEST_CASE("12.2.2 POST request with body emits HEADERS then DATA", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.submitRequest(makePostRequest("hello"));
  auto frames = decodeOutbound(result);

  bool foundHeaders = false;
  bool foundData = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::HEADERS) {
      REQUIRE_FALSE(f.header.hasEndStreamFlag());
      foundHeaders = true;
    }
    if (f.header.getType() == FrameType::DATA) {
      REQUIRE(f.header.hasEndStreamFlag());
      foundData = true;
    }
  }
  REQUIRE(foundHeaders);
  REQUIRE(foundData);
}

TEST_CASE("12.2.3 Pseudo headers before regular headers", "[submit]") {
  Http2Protocol proto;
  Hpack encTest;
  handshake(proto);

  Request req = makeGetRequest();
  req.headers["x-custom"] = "value";
  auto result = proto.submitRequest(req);
  REQUIRE(result.hasSubmittedStream);

  // Decode the outbound HEADERS frame and verify pseudo headers come first
  auto frames = decodeOutbound(result);
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::HEADERS) {
      // Decode with a test-side HPACK decoder
      std::vector<HeaderField> decodedHeaders;
      bool ok = encTest.decode(f.data, decodedHeaders);
      REQUIRE(ok);
      // Verify pseudo headers appear before regular headers
      bool seenRegular = false;
      for (auto& h : decodedHeaders) {
        if (!h.name.empty() && h.name[0] == ':') {
          REQUIRE_FALSE(seenRegular);
        } else {
          seenRegular = true;
        }
      }
      break;
    }
  }
}

// Test 12.2.4 removed: strict subset of test 12.1.10

TEST_CASE("12.2.5 Large header block generates CONTINUATION", "[submit]") {
  Http2Protocol proto;
  // Set a small MAX_FRAME_SIZE so that headers exceed a single frame
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), 16384}});

  // Add many custom headers to exceed max_frame_size
  Request req = makeGetRequest();
  for (int i = 0; i < 500; ++i) {
    req.headers["x-custom-header-" + std::to_string(i)] =
        std::string(100, 'a' + static_cast<char>(i % 26));
  }

  auto result = proto.submitRequest(req);
  REQUIRE(result.hasSubmittedStream);

  auto frames = decodeOutbound(result);
  bool foundHeaders = false;
  int continuationCount = 0;
  StreamId headerStreamId = 0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto& f = frames[i];
    if (f.header.getType() == FrameType::HEADERS) {
      foundHeaders = true;
      headerStreamId = f.header.streamId;
      // HEADERS must NOT have END_HEADERS when followed by CONTINUATIONs
      // (checked below after we know continuationCount > 0)
    }
    if (f.header.getType() == FrameType::CONTINUATION) {
      continuationCount++;
      // All CONTINUATION frames must target the same stream
      REQUIRE(f.header.streamId == headerStreamId);
      // Intermediate CONTINUATIONs must NOT have END_HEADERS
      if (i < frames.size() - 1 && frames[i + 1].header.getType() == FrameType::CONTINUATION) {
        REQUIRE_FALSE(f.header.hasEndHeadersFlag());
      }
    }
  }
  REQUIRE(foundHeaders);
  REQUIRE(continuationCount > 0);
  // HEADERS frame must NOT have END_HEADERS when CONTINUATIONs follow
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::HEADERS) {
      REQUIRE_FALSE(f.header.hasEndHeadersFlag());
      break;
    }
  }
  // Last frame should have END_HEADERS
  auto& last = frames.back();
  REQUIRE((last.header.getType() == FrameType::CONTINUATION ||
           last.header.getType() == FrameType::HEADERS));
  REQUIRE(last.header.hasEndHeadersFlag());

  // Reassemble HPACK block from HEADERS + CONTINUATION and decode
  ByteBuffer reassembled = ByteBuffer(64u);
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::HEADERS || f.header.getType() == FrameType::CONTINUATION) {
      reassembled.append(f.data.begin(), f.data.end());
    }
  }
  Hpack decoder;
  std::vector<HeaderField> decoded;
  REQUIRE(decoder.decode(reassembled, decoded));

  // Pseudo-headers must appear before regular headers
  bool seenRegular = false;
  for (auto& h : decoded) {
    if (!h.name.empty() && h.name[0] == ':') {
      REQUIRE_FALSE(seenRegular);
    } else {
      seenRegular = true;
    }
  }

  // All original request headers must be present
  for (auto& kv : req.headers) {
    bool found = false;
    for (auto& h : decoded) {
      if (h.name == kv.first && h.value == kv.second) {
        found = true;
        break;
      }
    }
    REQUIRE(found);
  }
}

TEST_CASE("12.2.6 Large body chunked into multiple DATA frames", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  // Body larger than max_frame_size (16384)
  std::string body(50000, 'A');
  auto result = proto.submitRequest(makePostRequest(body));
  REQUIRE(result.hasSubmittedStream);

  auto frames = decodeOutbound(result);
  std::uint32_t totalData = 0;
  int dataFrameCount = 0;
  bool lastHasEndStream = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      REQUIRE(f.header.payloadLength <= 16384);
      totalData += f.header.payloadLength;
      dataFrameCount++;
      lastHasEndStream = f.header.hasEndStreamFlag();
    }
  }
  REQUIRE(dataFrameCount > 1);
  REQUIRE(totalData == 50000);
  REQUIRE(lastHasEndStream);
}

TEST_CASE("12.3.1 GET request: stream is HALF_CLOSED_LOCAL", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.submitRequest(makeGetRequest());
  REQUIRE(proto.activeStreamCount() == 1);
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_OPENED));
}

TEST_CASE("12.3.2 POST request: stream starts OPEN", "[submit]") {
  Http2Protocol proto;
  // Use small window to keep body pending
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 2}});

  auto result = proto.submitRequest(makePostRequest("HelloWorld"));
  REQUIRE(result.hasSubmittedStream);
  REQUIRE(proto.activeStreamCount() == 1);
  // Body not fully sent — stream is OPEN (not half-closed)
  // Verify by checking that no END_STREAM was sent
  auto frames = decodeOutbound(result);
  bool headersHasEndStream = false;
  bool dataHasEndStream = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::HEADERS && f.header.hasEndStreamFlag()) {
      headersHasEndStream = true;
    }
    if (f.header.getType() == FrameType::DATA && f.header.hasEndStreamFlag()) {
      dataHasEndStream = true;
    }
  }
  REQUIRE_FALSE(headersHasEndStream);
  REQUIRE_FALSE(dataHasEndStream);
}

TEST_CASE("12.3.3 POST with body fully sent: HALF_CLOSED_LOCAL", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.submitRequest(makePostRequest("small"));
  REQUIRE(result.hasSubmittedStream);

  // Small body fits in window, should be sent immediately
  auto frames = decodeOutbound(result);
  bool dataHasEndStream = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA && f.header.hasEndStreamFlag()) {
      dataHasEndStream = true;
    }
  }
  REQUIRE(dataHasEndStream);
}

TEST_CASE("12.3.4 Empty body POST sends END_STREAM on HEADERS", "[submit]") {
  Http2Protocol proto;
  handshake(proto);

  Request req;
  req.headers[":method"] = "POST";
  req.headers[":scheme"] = "https";
  req.headers[":path"] = "/";
  req.headers[":authority"] = "example.com";
  req.body = "";
  auto result = proto.submitRequest(req);
  REQUIRE(result.hasSubmittedStream);

  auto frames = decodeOutbound(result);
  bool headersHasEndStream = false;
  int dataFrameCount = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::HEADERS) {
      headersHasEndStream = f.header.hasEndStreamFlag();
    }
    if (f.header.getType() == FrameType::DATA) {
      dataFrameCount++;
    }
  }
  // Empty body → END_STREAM on HEADERS, no DATA frames
  REQUIRE(headersHasEndStream);
  REQUIRE(dataFrameCount == 0);
}

// =============================================================================
// Section 13: CancelStream [cancel]
// =============================================================================

TEST_CASE("13.1 Cancel active stream", "[cancel]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = proto.cancelStream(sid, ErrorCode::CANCEL);

  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::CANCEL);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_CLOSED));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("13.2 Cancel unknown stream", "[cancel]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.cancelStream(999, ErrorCode::CANCEL);
  REQUIRE(result.outbound.empty());
  REQUIRE(result.events.empty());
}

TEST_CASE("13.3 Cancel after Close", "[cancel]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  proto.close();

  auto result = proto.cancelStream(sid, ErrorCode::CANCEL);
  REQUIRE(result.outbound.empty());
  REQUIRE(result.events.empty());
}

TEST_CASE("13.4 Cancel with various error codes", "[cancel]") {
  for (auto code : {ErrorCode::CANCEL, ErrorCode::INTERNAL_ERROR, ErrorCode::REFUSED_STREAM}) {
    Http2Protocol proto;
    handshake(proto);
    StreamId sid = submitSimpleGet(proto);

    auto result = proto.cancelStream(sid, code);
    auto frames = decodeOutbound(result);
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::RST_STREAM) {
        RstStreamFrame rst(f);
        REQUIRE(static_cast<ErrorCode>(rst.errorCode) == code);
      }
    }
  }
}

// =============================================================================
// Section 14: Response Header Validation [response_headers]
// =============================================================================

TEST_CASE("14.1.1 Missing :status pseudo header", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{"content-type", "text/plain"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.1.2 Invalid :status value (non-numeric)", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "abc"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.1.3 Invalid :status value (too short)", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "20"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.1.4 Invalid :status value (too long)", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "2000"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.1.5 Duplicate :status", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {":status", "200"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.1.6 Unknown pseudo header in response", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {":method", "GET"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.1.7 Pseudo header after regular header", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{"content-type", "text/plain"}, {":status", "200"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.1.8 Empty HPACK header block", "[response_headers]") {
  // An empty HPACK block decodes to an empty headers vector.
  // validateResponseHeaderBlock rejects empty headers (no :status).
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  ByteBuffer raw = makeRawFrame(FrameType::HEADERS,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::END_HEADERS) |
                                              static_cast<uint8_t>(FrameFlag::END_STREAM)),
                           sid,
                           std::string());  // 0-byte HPACK block
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.2.1 Uppercase header name", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"Content-Type", "text/html"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.2.2 Mixed case header name", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"Content-type", "text/html"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.2.3 Empty header name", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"", "value"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.3.1 Connection-specific headers rejected", "[response_headers]") {
  // RFC 9113 §8.2.2: connection, keep-alive, proxy-connection,
  // transfer-encoding, upgrade must not appear in HTTP/2 responses.
  struct Case {
    const char* name;
    const char* value;
  };
  Case cases[] = {
      {"connection", "keep-alive"},
      {"keep-alive", "timeout=5"},
      {"proxy-connection", "keep-alive"},
      {"transfer-encoding", "chunked"},
      {"upgrade", "websocket"},
  };
  for (auto& c : cases) {
    Http2Protocol proto;
    Hpack enc;
    handshake(proto);
    StreamId sid = submitSimpleGet(proto);

    std::vector<HeaderField> headers = {{":status", "200"}, {c.name, c.value}};
    ByteBuffer encoded = enc.encode(headers);
    ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
    auto result = feed(proto, raw);
    requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
  }
}

TEST_CASE("14.3.2 te header in response rejected", "[response_headers]") {
  // RFC 9113 §8.2.2: TE MAY only be present in requests, not responses.
  // Both "trailers" and other values are rejected.
  for (const char* value : {"trailers", "gzip"}) {
    Http2Protocol proto;
    Hpack enc;
    handshake(proto);
    StreamId sid = submitSimpleGet(proto);

    std::vector<HeaderField> headers = {{":status", "200"}, {"te", value}};
    ByteBuffer encoded = enc.encode(headers);
    ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
    auto result = feed(proto, raw);
    requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
  }
}

TEST_CASE("14.4.1 100 Continue followed by 200 OK", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // 100 Continue (no END_STREAM)
  auto r1 = feed(proto, makeResponseHeaders(enc, sid, 100, {}, false));
  REQUIRE(hasEvent(r1, ProtocolEventType::STREAM_HEADERS_RECEIVED));

  // 200 OK (END_STREAM)
  auto r2 = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(hasEvent(r2, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  REQUIRE(hasEvent(r2, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(r2, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getStatusCode() == 200);
}

TEST_CASE("14.4.2 103 Early Hints", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto r1 = feed(
      proto,
      makeResponseHeaders(enc, sid, 103, {{"link", "</style.css>; rel=preload; as=style"}}, false));
  REQUIRE(hasEvent(r1, ProtocolEventType::STREAM_HEADERS_RECEIVED));

  auto r2 = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(hasEvent(r2, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("14.4.3 1xx with END_STREAM", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeResponseHeaders(enc, sid, 100, {}, true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.4.4 Multiple 1xx before final response", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 100, {}, false));
  feed(proto, makeResponseHeaders(enc, sid, 103, {}, false));
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("14.5.1 Trailers with END_STREAM", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "body", false));

  // Trailers
  std::vector<HeaderField> trailers = {{"grpc-status", "0"}};
  ByteBuffer encoded = enc.encode(trailers);
  HeadersFrame tf(sid);
  tf.data = std::move(encoded);
  tf.header.payloadLength = tf.data.size();
  tf.header.setEndHeadersFlag();
  tf.header.setEndStreamFlag();
  auto result = feed(proto, tf.toBytes());
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE_FALSE(completed.response.trailers.empty());
  REQUIRE(completed.response.trailers[0].name == "grpc-status");
  REQUIRE(completed.response.trailers[0].value == "0");
}

TEST_CASE("14.5.2 Trailers without END_STREAM", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "body", false));

  std::vector<HeaderField> trailers = {{"grpc-status", "0"}};
  ByteBuffer encoded = enc.encode(trailers);
  HeadersFrame tf(sid);
  tf.data = std::move(encoded);
  tf.header.payloadLength = tf.data.size();
  tf.header.setEndHeadersFlag();
  // Deliberately NOT setting END_STREAM
  auto result = feed(proto, tf.toBytes());
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("14.5.3 Trailers with pseudo header", "[response_headers]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "body", false));

  std::vector<HeaderField> trailers = {{":status", "200"}};
  ByteBuffer encoded = enc.encode(trailers);
  HeadersFrame tf(sid);
  tf.data = std::move(encoded);
  tf.header.payloadLength = tf.data.size();
  tf.header.setEndHeadersFlag();
  tf.header.setEndStreamFlag();
  auto result = feed(proto, tf.toBytes());
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

// =============================================================================
// Section 15: Flow Control Integration [flow_control]
// =============================================================================

TEST_CASE("15.1 Inbound DATA consumes connection window", "[flow_control]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  const std::uint32_t dataSize = 8192;
  std::string payload(dataSize, 'X');
  auto result = feed(proto, makeDataFrame(sid, payload, false));
  auto frames = decodeOutbound(result);
  std::uint32_t totalConnIncrement = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::WINDOW_UPDATE && f.header.streamId == 0) {
      WindowUpdateFrame wu(f);
      totalConnIncrement += wu.windowSizeIncrement;
    }
  }
  // Refund must equal the consumed bytes
  REQUIRE(totalConnIncrement == dataSize);
}

TEST_CASE("15.2 Inbound DATA consumes stream window", "[flow_control]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  const std::uint32_t dataSize = 8192;
  std::string payload(dataSize, 'X');
  auto result = feed(proto, makeDataFrame(sid, payload, false));
  auto frames = decodeOutbound(result);
  std::uint32_t totalStreamIncrement = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::WINDOW_UPDATE && f.header.streamId == sid) {
      WindowUpdateFrame wu(f);
      totalStreamIncrement += wu.windowSizeIncrement;
    }
  }
  // Refund must equal the consumed bytes
  REQUIRE(totalStreamIncrement == dataSize);
}

TEST_CASE("15.3 Inbound flow control: large body fully consumed", "[flow_control]") {
  // The implementation auto-refills the inflow window after each DATA frame.
  // Verify that feeding many DATA frames totaling more than the initial
  // window (65535) works correctly — the protocol keeps issuing WINDOW_UPDATEs.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // Feed 96KB of data in 16384-byte chunks (exceeds 65535 initial window)
  const std::uint32_t chunkSize = 16384;
  const int numChunks = 6;
  std::string chunk(chunkSize, 'Y');
  std::uint32_t totalConnRefund = 0;
  for (int i = 0; i < numChunks; ++i) {
    bool end = (i == numChunks - 1);
    auto result = feed(proto, makeDataFrame(sid, chunk, end));
    REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
    auto frames = decodeOutbound(result);
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::WINDOW_UPDATE && f.header.streamId == 0) {
        WindowUpdateFrame wu(f);
        totalConnRefund += wu.windowSizeIncrement;
      }
    }
  }
  // Total refund must equal total data consumed
  REQUIRE(totalConnRefund == chunkSize * numChunks);
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("15.4 Padded DATA counts full payload including padding in flow control",
          "[flow_control]") {
  // RFC 9113 §6.1: "The entire DATA frame payload is included in flow
  // control, including the Pad Length and Padding fields if present."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100}});
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // 10 bytes of actual data + 91 bytes of padding.
  // makeDataFrame sets payloadLength = 1 (pad_length) + 10 + 91 = 102.
  // Stream inflow window is 100, so 102 > 100 → FLOW_CONTROL_ERROR.
  // If padding were NOT counted, only 10 bytes consumed → no error (wrong).
  auto result = feed(proto, makeDataFrame(sid, std::string(10, 'X'), false, 91));
  requireConnectionError(result, ErrorCode::FLOW_CONTROL_ERROR, proto);
}

TEST_CASE("15.5 Padded DATA WINDOW_UPDATE refunds full payload including padding",
          "[flow_control]") {
  // After receiving padded DATA, the WINDOW_UPDATE must refund the full
  // payload size (including Pad Length + Padding), not just the data portion.
  // Use payload > INFLOW_MIN_REFRESH (4096) to ensure WU is emitted.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // 4000 bytes data + 200 bytes padding. payloadLength = 1 + 4000 + 200 = 4201.
  const std::uint32_t expectedPayload = 4201;
  // Don't set END_STREAM so stream stays open for stream-level WU
  auto result = feed(proto, makeDataFrame(sid, std::string(4000, 'X'), false, 200));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));

  auto frames = decodeOutbound(result);
  std::uint32_t connRefund = 0;
  std::uint32_t streamRefund = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::WINDOW_UPDATE) {
      WindowUpdateFrame wu(f);
      if (f.header.streamId == 0) {
        connRefund += wu.windowSizeIncrement;
      } else if (f.header.streamId == sid) {
        streamRefund += wu.windowSizeIncrement;
      }
    }
  }
  // Refund must equal the full payload (pad_length byte + data + padding)
  REQUIRE(connRefund == expectedPayload);
  REQUIRE(streamRefund == expectedPayload);
}

TEST_CASE("15.6 Outbound body respects stream window", "[flow_control]") {
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 3}});

  auto result = proto.submitRequest(makePostRequest("HelloWorld"));
  auto frames = decodeOutbound(result);
  std::uint32_t dataBytes = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes += f.header.payloadLength;
    }
  }
  REQUIRE(dataBytes == 3);
}

// Test 15.7 removed: exact duplicate of test 12.3.4

TEST_CASE("15.8 Connection and stream windows independent", "[flow_control]") {
  // Verify that min(stream_window, connection_window) determines the limit.
  // Case 1: stream window (3) < connection window (65535) → limited by stream
  {
    Http2Protocol proto;
    handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 3}});
    auto result = proto.submitRequest(makePostRequest(std::string(100, 'A')));
    auto frames = decodeOutbound(result);
    std::uint32_t dataBytes = 0;
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::DATA) {
        dataBytes += f.header.payloadLength;
      }
    }
    REQUIRE(dataBytes == 3);  // stream window is the bottleneck
  }
  // Case 2: connection window (65535) < stream window (100000) → limited by connection
  {
    Http2Protocol proto;
    handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100000}});
    auto result = proto.submitRequest(makePostRequest(std::string(70000, 'B')));
    auto frames = decodeOutbound(result);
    std::uint32_t dataBytes = 0;
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::DATA) {
        dataBytes += f.header.payloadLength;
      }
    }
    REQUIRE(dataBytes == 65535);  // connection window is the bottleneck
  }
}

TEST_CASE("15.9 Both stream and connection END_STREAM -> CLOSED", "[flow_control]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);

  // POST with small body, fully sent
  auto submitResult = proto.submitRequest(makePostRequest("data"));
  StreamId sid = submitResult.submittedStreamId;

  // Receive 200 + END_STREAM from server
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_CLOSED));
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("15.10 Multiple streams competing for connection flow control window", "[flow_control]") {
  Http2Protocol proto;
  // Large stream window so connection window (65535) is the shared bottleneck
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100000}});

  // Submit 3 POST requests with 30000-byte bodies each (total 90000 > 65535).
  // The connection window is shared, so the total DATA sent across all streams
  // must not exceed 65535.
  auto r1 = proto.submitRequest(makePostRequest(std::string(30000, 'A')));
  auto r2 = proto.submitRequest(makePostRequest(std::string(30000, 'B')));
  auto r3 = proto.submitRequest(makePostRequest(std::string(30000, 'C')));
  REQUIRE(r1.hasSubmittedStream);
  REQUIRE(r2.hasSubmittedStream);
  REQUIRE(r3.hasSubmittedStream);

  // Total DATA across all streams must be <= 65535 (connection window)
  std::uint32_t totalData = 0;
  for (auto* result : {&r1, &r2, &r3}) {
    auto frames = decodeOutbound(*result);
    for (auto& f : frames) {
      if (f.header.getType() == FrameType::DATA) {
        totalData += f.header.payloadLength;
      }
    }
  }
  REQUIRE(totalData == 65535);
  REQUIRE(proto.activeStreamCount() == 3);  // none fully sent

  // Connection WINDOW_UPDATE should unblock remaining data
  auto r4 = feed(proto, makeWindowUpdate(0, 90000 - 65535));
  auto frames4 = decodeOutbound(r4);
  std::uint32_t remainingData = 0;
  for (auto& f : frames4) {
    if (f.header.getType() == FrameType::DATA) {
      remainingData += f.header.payloadLength;
    }
  }
  REQUIRE(remainingData == 90000 - 65535);
}

TEST_CASE("15.11 Body split across multiple WINDOW_UPDATE rounds", "[flow_control]") {
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 3}});

  auto submitResult = proto.submitRequest(makePostRequest("ABCDEFGHIJ"));  // 10 bytes
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  // 3 bytes sent initially
  auto frames0 = decodeOutbound(submitResult);
  std::uint32_t sent = 0;
  for (auto& f : frames0) {
    if (f.header.getType() == FrameType::DATA) {
      sent += f.header.payloadLength;
    }
  }
  REQUIRE(sent == 3);

  // Round 2: WINDOW_UPDATE for 3 more bytes
  auto r1 = feed(proto, makeWindowUpdate(sid, 3));
  auto frames1 = decodeOutbound(r1);
  std::uint32_t sent1 = 0;
  for (auto& f : frames1) {
    if (f.header.getType() == FrameType::DATA) {
      sent1 += f.header.payloadLength;
    }
  }
  REQUIRE(sent1 == 3);

  // Round 3: WINDOW_UPDATE for 4 more bytes — remaining 4 bytes sent
  auto r2 = feed(proto, makeWindowUpdate(sid, 4));
  auto frames2 = decodeOutbound(r2);
  std::uint32_t sent2 = 0;
  bool lastEndStream = false;
  for (auto& f : frames2) {
    if (f.header.getType() == FrameType::DATA) {
      sent2 += f.header.payloadLength;
      lastEndStream = f.header.hasEndStreamFlag();
    }
  }
  REQUIRE(sent2 == 4);
  REQUIRE(lastEndStream);
}

// =============================================================================
// Section 16: Stream State Machine [stream_state]
// =============================================================================

TEST_CASE("16.1 GET: IDLE -> HALF_CLOSED_LOCAL -> CLOSED", "[stream_state]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  auto submitResult = proto.submitRequest(makeGetRequest());
  StreamId sid = submitResult.submittedStreamId;
  REQUIRE(proto.activeStreamCount() == 1);

  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_CLOSED));
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("16.2 POST: IDLE -> OPEN -> HALF_CLOSED_LOCAL -> CLOSED", "[stream_state]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  auto submitResult = proto.submitRequest(makePostRequest("data"));
  StreamId sid = submitResult.submittedStreamId;
  REQUIRE(proto.activeStreamCount() == 1);

  // Body sent with END_STREAM, now HALF_CLOSED_LOCAL
  // Receive 200 + END_STREAM → CLOSED
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_CLOSED));
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("16.3 Server closes before client: HALF_CLOSED_REMOTE", "[stream_state]") {
  Http2Protocol proto;
  Hpack enc;
  // Use small window to keep body pending
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 2}});

  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  StreamId sid = submitResult.submittedStreamId;
  // Only 2 bytes sent, stream is OPEN (body pending)

  // Server sends 200 + END_STREAM → HALF_CLOSED_REMOTE (body still pending)
  auto r1 = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(hasEvent(r1, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  auto hdrEvt = findEvent(r1, ProtocolEventType::STREAM_HEADERS_RECEIVED);
  REQUIRE(hdrEvt.response.getStatusCode() == 200);
  // Stream not yet closed — our body is still pending
  REQUIRE(proto.activeStreamCount() == 1);

  // Now send WINDOW_UPDATE to unblock remaining body
  auto r2 = feed(proto, makeWindowUpdate(sid, 100));
  // Body should now be fully sent → CLOSED
  REQUIRE(hasEvent(r2, ProtocolEventType::STREAM_CLOSED));
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("16.4 RST_STREAM closes stream immediately", "[stream_state]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  REQUIRE(proto.activeStreamCount() == 1);

  feed(proto, makeRstStream(sid, ErrorCode::CANCEL));
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("16.5 Multiple streams concurrent lifecycle", "[stream_state]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");
  REQUIRE(proto.activeStreamCount() == 3);

  // Complete out of order
  completeResponse(proto, enc, s3);
  REQUIRE(proto.activeStreamCount() == 2);
  completeResponse(proto, enc, s1);
  REQUIRE(proto.activeStreamCount() == 1);
  completeResponse(proto, enc, s5);
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("16.6 Stream error on one stream does not affect others", "[stream_state]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");
  REQUIRE(proto.activeStreamCount() == 3);

  // RST_STREAM on s3 — only s3 should be closed
  auto result = feed(proto, makeRstStream(s3, ErrorCode::INTERNAL_ERROR));
  REQUIRE(proto.activeStreamCount() == 2);
  REQUIRE_FALSE(proto.isClosed());

  // s1 and s5 can still complete
  completeResponse(proto, enc, s1);
  REQUIRE(proto.activeStreamCount() == 1);
  completeResponse(proto, enc, s5);
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("16.7 Concurrent slot freed after stream close", "[stream_state]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 2}});

  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Third request rejected — at limit
  auto r3 = proto.submitRequest(makeGetRequest("/3"));
  REQUIRE_FALSE(r3.hasSubmittedStream);

  // Complete s1 → frees a slot
  completeResponse(proto, enc, s1);
  REQUIRE(proto.activeStreamCount() == 1);

  // Now third request should succeed
  auto r4 = proto.submitRequest(makeGetRequest("/3"));
  REQUIRE(r4.hasSubmittedStream);
  REQUIRE(proto.activeStreamCount() == 2);
  (void) s3;
}

// =============================================================================
// Section 17: HPACK State Consistency [hpack_state]
// =============================================================================

TEST_CASE("17.1 HPACK state preserved after HEADERS on closed stream", "[hpack_state]") {
  // RFC 9113 §4.3: every header block MUST be decoded even if the stream
  // is already closed, to keep the HPACK dynamic table in sync.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Complete stream s1
  completeResponse(proto, enc, s1);
  REQUIRE(proto.activeStreamCount() == 1);

  // Server sends HEADERS on closed stream s1 with some headers that would
  // update the HPACK dynamic table. This must be decoded even if discarded.
  feed(proto, makeResponseHeaders(enc, s1, 200, {{"x-dynamic", "value1"}}, true));

  // Now complete stream s3 — decoder must still work
  auto result = feed(proto, makeResponseHeaders(enc, s3, 200, {{"x-dynamic", "value2"}}, true));
  // If HPACK state was preserved, this should succeed
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

// Test 17.2 removed: duplicate of test 20.3 (HPACK after RST_STREAM)

TEST_CASE("17.3 HPACK decode error is connection error", "[hpack_state]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Corrupt HPACK data: invalid index reference
  ByteBuffer badHpack = ByteBuffer(4u);
  badHpack.append<std::uint8_t>(0xFF);  // dynamic table index 127+
  badHpack.append<std::uint8_t>(0xFF);
  badHpack.append<std::uint8_t>(0xFF);
  badHpack.append<std::uint8_t>(0x7F);  // very large index
  ByteBuffer raw = makeResponseHeadersRaw(badHpack, sid, true);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::COMPRESSION_ERROR, proto);
}

TEST_CASE("17.4 HPACK state after multiple streams", "[hpack_state]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);

  // Open and complete 5 streams with varying custom headers to build up
  // the HPACK dynamic table. Each stream adds a new dynamic table entry.
  for (int i = 0; i < 5; ++i) {
    StreamId sid = submitSimpleGet(proto, "/" + std::to_string(i));
    std::string headerName = "x-dyn-" + std::to_string(i);
    std::string headerVal = "value-" + std::to_string(i);
    feed(proto, makeResponseHeaders(enc, sid, 200, {{headerName, headerVal}}, true));
  }

  // 6th stream should decode correctly using accumulated HPACK state.
  // The encoder will reference dynamic table entries created by previous
  // streams, so if the decoder's table is out of sync this will fail.
  StreamId s6 = submitSimpleGet(proto, "/6");
  auto result = feed(proto, makeResponseHeaders(enc, s6, 200, {{"x-final", "test"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getHeader("x-final") == "test");
}

TEST_CASE("17.5 HPACK state after ghost stream CONTINUATION", "[hpack_state]") {
  // RFC 9113 §4.3: when a stream is already closed and server sends
  // HEADERS without END_HEADERS, the ghost header block path must decode
  // the full CONTINUATION sequence to keep HPACK dynamic table in sync.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Complete s1 first — it's now removed from the stream map
  completeResponse(proto, enc, s1);
  REQUIRE(proto.activeStreamCount() == 1);

  // Encode headers with dynamic table entries, split into two parts.
  // Server sends this on the already-closed s1, requiring ghost path.
  std::vector<HeaderField> headers = {{":status", "200"}, {"x-ghost", "value-for-dynamic-table"}};
  ByteBuffer encoded = enc.encode(headers);
  std::uint32_t half = encoded.size() / 2;
  ByteBuffer part1 = ByteBuffer::from(encoded.begin(), encoded.begin() + half);
  ByteBuffer part2 = ByteBuffer::from(encoded.begin() + half, encoded.end());

  // HEADERS without END_HEADERS on closed s1 → ghost_header_block_ initialized
  feed(proto, makeResponseHeadersRaw(part1, s1, false, false));
  REQUIRE_FALSE(proto.isClosed());

  // CONTINUATION completes the ghost header block — HPACK must decode it
  auto contResult = feed(proto, makeContinuation(s1, part2, true));
  REQUIRE_FALSE(proto.isClosed());

  // Now s3 uses the same encoder — if HPACK state is correct, this works
  auto result = feed(proto, makeResponseHeaders(enc, s3, 200, {{"x-after-ghost", "ok"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

// =============================================================================
// Section 18: Half-Closed State Violations [half_closed]
// =============================================================================

TEST_CASE("18.1 HEADERS on half-closed (remote) stream", "[half_closed]") {
  Http2Protocol proto;
  Hpack enc;
  // Use small window to keep body pending so stream stays in map
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 2}});

  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  StreamId sid = submitResult.submittedStreamId;
  // Only 2 bytes sent, stream is OPEN

  // Server sends HEADERS + END_STREAM → HALF_CLOSED_REMOTE (body still pending)
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  REQUIRE(proto.activeStreamCount() == 1);  // stream still in map

  // Server sends another HEADERS on the same stream → stream error STREAM_CLOSED
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::STREAM_CLOSED);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("18.2 DATA on half-closed (remote) stream", "[half_closed]") {
  Http2Protocol proto;
  Hpack enc;
  // Use small window to keep body pending on stream
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 2}});

  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  StreamId sid = submitResult.submittedStreamId;

  // Server sends HEADERS + END_STREAM → HALF_CLOSED_REMOTE
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));

  // Server sends DATA on this stream → stream error STREAM_CLOSED
  auto result = feed(proto, makeDataFrame(sid, "extra", false));
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::STREAM_CLOSED);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("18.3 WINDOW_UPDATE on half-closed (remote) is allowed", "[half_closed]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 2}});

  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  StreamId sid = submitResult.submittedStreamId;

  // Server sends HEADERS + END_STREAM
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));

  // WINDOW_UPDATE on the stream — allowed per RFC 9113 §5.1
  auto result = feed(proto, makeWindowUpdate(sid, 100));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("18.4 RST_STREAM on half-closed (remote) is allowed", "[half_closed]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 2}});

  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  StreamId sid = submitResult.submittedStreamId;

  // Server sends HEADERS + END_STREAM
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));

  // RST_STREAM — allowed, stream is reset
  auto result = feed(proto, makeRstStream(sid, ErrorCode::CANCEL));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

// =============================================================================
// Section 19: Field Value Validation [field_validation]
// RFC 9113 §8.2.1: field name/value character restrictions.
// =============================================================================

TEST_CASE("19.1 Header value containing NUL (0x00)", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::string val = "foo";
  val += '\x00';
  val += "bar";
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-data", val}}, true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("19.2 Header value containing CR (0x0D)", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::string val = "foo\rbar";
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-data", val}}, true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("19.3 Header value containing LF (0x0A)", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::string val = "foo\nbar";
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-data", val}}, true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("19.4 Header name with control character", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // RFC 9113 §8.2.1: name MUST NOT contain chars in 0x00-0x20
  std::string name = "x-da";
  name += '\x01';
  name += "ta";
  std::vector<HeaderField> headers = {{":status", "200"}, {name, "foo"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("19.5 Header name with high byte (0x80+)", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // RFC 9113 §8.2.1: name MUST NOT contain chars in 0x7f-0xff
  std::string name = "x-d\xc3\xa4t\xc3\xa4";  // "x-dätä" in UTF-8
  std::vector<HeaderField> headers = {{":status", "200"}, {name, "foo"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("19.6 Header value starting with space", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // RFC 9113 §8.2.1: value MUST NOT start or end with SP/HTAB
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-data", " leading"}}, true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("19.7 Header value ending with tab", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::string val = "trailing\t";
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-data", val}}, true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("19.8 Valid header value passes through", "[field_validation]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-data", "hello world"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

// =============================================================================
// Section 20: RST_STREAM Edge Cases [rst_stream_edge]
// =============================================================================

TEST_CASE("20.1 No RST_STREAM in response to RST_STREAM", "[rst_stream_edge]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeRstStream(sid, ErrorCode::CANCEL));
  // Must NOT send RST_STREAM in response (RFC 9113 §6.4)
  auto frames = decodeOutbound(result);
  for (auto& f : frames) {
    REQUIRE_FALSE(f.header.getType() == FrameType::RST_STREAM);
  }
}

TEST_CASE("20.2 DATA after local RST_STREAM: connection flow control accounted",
          "[rst_stream_edge]") {
  // RFC 9113 §5.1: DATA on a closed stream must still count against the
  // connection flow-control window and be properly refunded.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  proto.cancelStream(sid, ErrorCode::CANCEL);
  REQUIRE(proto.activeStreamCount() == 0);

  // Feed DATA on cancelled stream — must be silently consumed + refunded
  const std::uint32_t dataSize = 5000;
  auto result = feed(proto, makeDataFrame(sid, std::string(dataSize, 'X'), false));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE_FALSE(proto.isClosed());

  auto frames = decodeOutbound(result);
  // No RST_STREAM sent back (stream already reset)
  std::uint32_t connRefund = 0;
  for (auto& f : frames) {
    REQUIRE_FALSE(f.header.getType() == FrameType::RST_STREAM);
    if (f.header.getType() == FrameType::WINDOW_UPDATE && f.header.streamId == 0) {
      WindowUpdateFrame wu(f);
      connRefund += wu.windowSizeIncrement;
    }
  }
  // Connection window must be refunded exactly
  REQUIRE(connRefund == dataSize);
}

TEST_CASE("20.3 HEADERS after RST_STREAM sent — HPACK still decoded", "[rst_stream_edge]") {
  // RFC 9113 §5.1: after sending RST_STREAM, endpoint must be prepared
  // to receive frames. HEADERS must still be HPACK-decoded.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Cancel s1 — sends RST_STREAM
  proto.cancelStream(s1, ErrorCode::CANCEL);

  // Server sends HEADERS on cancelled s1 with dynamic table updates
  feed(proto, makeResponseHeaders(enc, s1, 200, {{"x-post-rst", "decoded"}}, true));

  // s3 must still decode correctly using same HPACK state
  auto result = feed(proto, makeResponseHeaders(enc, s3, 200, {{"x-final", "works"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getHeader("x-final") == "works");
}

// =============================================================================
// Section 21: Unknown and Extension Frames [unknown_frame]
// =============================================================================

TEST_CASE("21.1 Unknown frame type ignored, parser aligned", "[unknown_frame]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer raw = makeRawFrame(static_cast<FrameType>(0xFE), 0, 0, std::string("test"));
  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::UNKNOWN_FRAME_RECEIVED));
  auto evt = findEvent(result, ProtocolEventType::UNKNOWN_FRAME_RECEIVED);
  REQUIRE(evt.frameType == static_cast<FrameType>(0xFE));
  REQUIRE(evt.streamId == 0);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));

  // Follow with valid PING to prove parser alignment
  auto pingResult = feed(proto, makePing("UNKNTEST"));
  REQUIRE(hasEvent(pingResult, ProtocolEventType::PING_RECEIVED));
}

TEST_CASE("21.2 Unknown frame type on stream, parser aligned", "[unknown_frame]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer raw = makeRawFrame(static_cast<FrameType>(0xFD), 0, 1, std::string("test-payload"));
  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::UNKNOWN_FRAME_RECEIVED));
  auto evt = findEvent(result, ProtocolEventType::UNKNOWN_FRAME_RECEIVED);
  REQUIRE(evt.streamId == 1);

  // Follow with valid SETTINGS to prove parser alignment
  auto settingsResult = feed(proto, makeEmptyServerSettings());
  REQUIRE(hasEvent(settingsResult, ProtocolEventType::SETTINGS_RECEIVED));
}

TEST_CASE("21.3 Unknown frame type with various flags, parser aligned", "[unknown_frame]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer raw = makeRawFrame(static_cast<FrameType>(0xFD), 0xFF, 0, std::string("test"));
  auto result = feed(proto, raw);
  auto evt = findEvent(result, ProtocolEventType::UNKNOWN_FRAME_RECEIVED);
  REQUIRE(evt.flags == 0xFF);

  // Follow with valid PING to prove parser alignment
  auto pingResult = feed(proto, makePing("FLAGUNKN"));
  REQUIRE(hasEvent(pingResult, ProtocolEventType::PING_RECEIVED));
}

// =============================================================================
// Section 22: Error Handling [error_handling]
// =============================================================================

TEST_CASE("22.1 Connection error sends GOAWAY and closes", "[error_handling]") {
  Http2Protocol proto;
  handshake(proto);
  submitSimpleGet(proto);

  // Trigger connection error: SETTINGS on non-zero stream
  ByteBuffer raw = makeRawFrame(FrameType::SETTINGS, 0, 1, std::string());
  auto result = feed(proto, raw);

  auto frames = decodeOutbound(result);
  bool foundGoaway = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::GOAWAY) {
      foundGoaway = true;
    }
  }
  REQUIRE(foundGoaway);
  REQUIRE(hasEvent(result, ProtocolEventType::GOAWAY_SENT));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_FAILED));
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_CLOSED));
  REQUIRE(proto.isClosed());
}

TEST_CASE("22.2 Connection error sends only one GOAWAY", "[error_handling]") {
  Http2Protocol proto;
  handshake(proto);

  ByteBuffer bad = makeRawFrame(FrameType::SETTINGS, 0, 1, std::string());
  auto r1 = feed(proto, bad);
  auto frames1 = decodeOutbound(r1);
  int goawayCount = 0;
  for (auto& f : frames1) {
    if (f.header.getType() == FrameType::GOAWAY) {
      goawayCount++;
    }
  }
  REQUIRE(goawayCount == 1);

  // Connection is now closed. Further data is no-op.
  auto r2 = feed(proto, bad);
  REQUIRE(r2.outbound.empty());
}

TEST_CASE("22.3 Connection error records error code", "[error_handling]") {
  Http2Protocol proto;
  handshake(proto);

  // Trigger FRAME_SIZE_ERROR
  ByteBuffer raw = makeRawFrame(FrameType::SETTINGS, 0, 0, std::string(7, '\0'));
  feed(proto, raw);
  REQUIRE(proto.lastConnectionError() == ErrorCode::FRAME_SIZE_ERROR);
  REQUIRE_FALSE(proto.lastConnectionDebugData().empty());
}

TEST_CASE("22.4 Stream error sends RST_STREAM", "[error_handling]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // WINDOW_UPDATE increment=0 on s1 → stream error
  auto result = feed(proto, makeWindowUpdate(s1, 0));
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == s1) {
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  // s3 should still be active
  REQUIRE(proto.activeStreamCount() == 1);
  REQUIRE_FALSE(proto.isClosed());
  (void) s3;
}

TEST_CASE("22.5 cancelStream removes stream from map", "[error_handling]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  submitSimpleGet(proto, "/2");
  REQUIRE(proto.activeStreamCount() == 2);

  auto result = proto.cancelStream(s1, ErrorCode::CANCEL);
  REQUIRE(proto.activeStreamCount() == 1);
  // Verify RST_STREAM was sent
  auto frames = decodeOutbound(result);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == s1) {
      RstStreamFrame rst(f);
      REQUIRE(static_cast<ErrorCode>(rst.errorCode) == ErrorCode::CANCEL);
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
}

TEST_CASE("22.6 Connection error after transport closed omits GOAWAY", "[error_handling]") {
  Http2Protocol proto;
  handshake(proto);
  proto.onTransportClosed();

  // Connection is closed. Any feed is no-op.
  ByteBuffer settings = makeEmptyServerSettings();
  auto result = feed(proto, settings);
  REQUIRE(result.outbound.empty());
}

// Test 22.7 removed: exact duplicate of test 17.3 "HPACK decode error is connection error"

TEST_CASE("22.7 Multiple consecutive stream errors dont kill connection", "[error_handling]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");

  // Trigger stream error on each via WINDOW_UPDATE increment=0
  feed(proto, makeWindowUpdate(s1, 0));
  feed(proto, makeWindowUpdate(s3, 0));
  auto result = feed(proto, makeWindowUpdate(s5, 0));

  // Connection must still be alive
  REQUIRE_FALSE(proto.isClosed());
  REQUIRE(proto.activeStreamCount() == 0);
}

TEST_CASE("22.8 New stream can be opened after stream error", "[error_handling]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");

  // RST_STREAM kills s1
  feed(proto, makeRstStream(s1, ErrorCode::INTERNAL_ERROR));
  REQUIRE(proto.activeStreamCount() == 0);

  // Open a new stream — should work fine
  StreamId s3 = submitSimpleGet(proto, "/2");
  REQUIRE(proto.activeStreamCount() == 1);

  // Complete it normally
  completeResponse(proto, enc, s3);
  REQUIRE(proto.activeStreamCount() == 0);
}

// =============================================================================
// Section 23: Frame Size Enforcement [frame_size]
// =============================================================================

TEST_CASE("23.1 Frame exceeding local max frame size", "[frame_size]") {
  Http2Protocol proto;
  handshake(proto);

  // Default max frame size is 16384. Feed frame with 16385 bytes payload.
  std::string big(16385, 'X');
  ByteBuffer raw = makeRawFrame(static_cast<FrameType>(0xFE), 0, 0, big);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

TEST_CASE("23.2 Frame at exactly max frame size", "[frame_size]") {
  Http2Protocol proto;
  handshake(proto);

  std::string exact(16384, 'X');
  ByteBuffer raw = makeRawFrame(static_cast<FrameType>(0xFE), 0, 0, exact);
  auto result = feed(proto, raw);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

TEST_CASE("23.3 Peer MAX_FRAME_SIZE does not affect inbound limit", "[frame_size]") {
  Http2Protocol proto;
  // Peer sets MAX_FRAME_SIZE=32768, but OUR local max_frame_size (16384)
  // still governs inbound frame size enforcement.
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), 32768}});

  // Inbound frame with 16385 bytes should STILL be rejected
  // because our local limit remains 16384.
  std::string big(16385, 'X');
  ByteBuffer raw = makeRawFrame(static_cast<FrameType>(0xFE), 0, 0, big);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

TEST_CASE("23.4 HEADERS frame exceeding max frame size is connection error", "[frame_size]") {
  // RFC 9113 §4.2: frame carrying a field block that exceeds max frame size
  // MUST be treated as a connection error.
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Build HEADERS frame with payload > 16384 (default max frame size)
  std::string bigPayload(16385, 'X');
  ByteBuffer raw = makeRawFrame(FrameType::HEADERS,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::END_HEADERS) |
                                              static_cast<uint8_t>(FrameFlag::END_STREAM)),
                           sid,
                           bigPayload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

TEST_CASE("23.5 Server MAX_FRAME_SIZE change affects outbound DATA chunk size", "[frame_size]") {
  Http2Protocol proto;
  // Server advertises larger max frame size
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::MAX_FRAME_SIZE), 32768}});

  // POST with body larger than 16384 but smaller than 32768
  std::string body(24000, 'x');
  auto result = proto.submitRequest(makePostRequest(body));
  REQUIRE(result.hasSubmittedStream);

  auto frames = decodeOutbound(result);
  int dataFrameCount = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      dataFrameCount++;
      // Each DATA frame should be <= 32768
      REQUIRE(f.header.payloadLength <= 32768);
    }
  }
  // With 32768 max frame size, 24000 bytes should fit in a single DATA frame
  REQUIRE(dataFrameCount == 1);
}

TEST_CASE("23.6 CONTINUATION frame exceeding max frame size is connection error", "[frame_size]") {
  // RFC 9113 §4.2: applies to ALL frame types including CONTINUATION.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Start a HEADERS without END_HEADERS
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false, false));

  // Feed CONTINUATION with payload > 16384 (default max frame size)
  std::string bigPayload(16385, 'X');
  ByteBuffer bigSlice = ByteBuffer::from(bigPayload.begin(), bigPayload.end());
  ByteBuffer raw = makeRawFrame(
      FrameType::CONTINUATION, static_cast<uint8_t>(FrameFlag::END_HEADERS), sid, bigSlice);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

// =============================================================================
// Section 24: Edge Cases and Robustness [edge_cases]
// =============================================================================

TEST_CASE("24.1 Empty ReceiveBytes", "[edge_cases]") {
  Http2Protocol proto;
  handshake(proto);

  auto result = proto.receiveBytes(nullptr, 0);
  REQUIRE(result.events.empty());
  REQUIRE(result.outbound.empty());
}

TEST_CASE("24.2 Partial frame bytes", "[edge_cases]") {
  Http2Protocol proto;
  handshake(proto);

  // 5 bytes = incomplete frame header (need 9)
  std::uint8_t partial[] = {0, 0, 0, 4, 0};
  auto result = proto.receiveBytes(partial, 5);
  REQUIRE(result.events.empty());
  REQUIRE(result.outbound.empty());
}

TEST_CASE("24.3 Byte-at-a-time feeding", "[edge_cases]") {
  Http2Protocol proto;
  proto.start();

  // Feed server SETTINGS one byte at a time
  ByteBuffer settings = makeEmptyServerSettings();
  ProtocolStepResult settingsResult;
  for (std::uint32_t i = 0; i < settings.size(); ++i) {
    auto r = proto.receiveBytes(settings.begin() + i, 1);
    for (auto& e : r.events) {
      settingsResult.events.push_back(e);
    }
    for (auto& o : r.outbound) {
      settingsResult.outbound.push_back(std::move(o));
    }
  }
  // SETTINGS_RECEIVED should have been emitted
  REQUIRE(hasEvent(settingsResult, ProtocolEventType::SETTINGS_RECEIVED));

  // Verify SETTINGS ACK was sent
  auto frames = decodeOutbound(settingsResult);
  bool foundAck = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::SETTINGS && f.header.hasAckFlag()) {
      foundAck = true;
    }
  }
  REQUIRE(foundAck);

  // Feed SETTINGS ACK one byte at a time
  ByteBuffer ack = makeSettingsAck();
  ProtocolStepResult ackResult;
  for (std::uint32_t i = 0; i < ack.size(); ++i) {
    auto r = proto.receiveBytes(ack.begin() + i, 1);
    for (auto& e : r.events) {
      ackResult.events.push_back(e);
    }
  }
  REQUIRE(hasEvent(ackResult, ProtocolEventType::SETTINGS_ACK_RECEIVED));
  REQUIRE_FALSE(proto.isClosed());
}

TEST_CASE("24.4 Multiple frames in single ReceiveBytes", "[edge_cases]") {
  Http2Protocol proto;
  proto.start();

  // Concatenate server SETTINGS + SETTINGS ACK
  ByteBuffer settings = makeEmptyServerSettings();
  ByteBuffer ack = makeSettingsAck();
  ByteBuffer combined(settings.size() + ack.size());
  combined.append(settings.begin(), settings.end());
  combined.append(ack.begin(), ack.end());

  auto result = feed(proto, combined);
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_ACK_RECEIVED));
}

TEST_CASE("24.5 Rapid stream open and close", "[edge_cases]") {
  Http2Protocol proto;
  handshake(proto);

  for (int i = 0; i < 100; ++i) {
    StreamId sid = submitSimpleGet(proto);
    proto.cancelStream(sid, ErrorCode::CANCEL);
  }
  REQUIRE(proto.activeStreamCount() == 0);
  REQUIRE(proto.nextLocalStreamId() == 201);
}

// Test 24.6 removed: duplicate of tests 3.2.3 + 4.2.1

// Test 24.7 removed: duplicate of test 16.5 (concurrent stream lifecycle)

TEST_CASE("24.8 SETTINGS interleaved with requests", "[edge_cases]") {
  Http2Protocol proto;
  handshake(proto);
  submitSimpleGet(proto);

  ByteBuffer settings = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 50}});
  auto result = feed(proto, settings);
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));
  REQUIRE_FALSE(proto.isClosed());

  auto r = proto.submitRequest(makeGetRequest("/2"));
  REQUIRE(r.hasSubmittedStream);
}

TEST_CASE("24.9 Stream interleaving: responses arrive out of request order", "[edge_cases]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");

  // Stream 5 gets headers first
  feed(proto, makeResponseHeaders(enc, s5, 200, {}, false));

  // Stream 1 completes entirely
  auto r1 = feed(proto, makeResponseHeaders(enc, s1, 200, {}, true));
  REQUIRE(hasEvent(r1, ProtocolEventType::RESPONSE_COMPLETED));
  REQUIRE(proto.activeStreamCount() == 2);

  // Stream 3 gets headers, then data
  feed(proto, makeResponseHeaders(enc, s3, 200, {}, false));
  feed(proto, makeDataFrame(s3, "data3", true));
  REQUIRE(proto.activeStreamCount() == 1);

  // Stream 5 finishes last
  auto r5 = feed(proto, makeDataFrame(s5, "data5", true));
  REQUIRE(hasEvent(r5, ProtocolEventType::RESPONSE_COMPLETED));
  REQUIRE(proto.activeStreamCount() == 0);
}

// =============================================================================
// Section 25: Missing RFC Scenario Coverage [rfc_coverage]
// =============================================================================

// ---------------------------------------------------------------------------
// 25.1  101 Switching Protocols MUST be rejected (RFC 9113 §8.6)
// ---------------------------------------------------------------------------

TEST_CASE("25.1 101 Switching Protocols rejected", "[rfc_coverage]") {
  // RFC 9113 §8.6: "HTTP/2 does not support the 101 (Switching Protocols)
  // informational status code."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeResponseHeaders(enc, sid, 101, {}, false));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

// ---------------------------------------------------------------------------
// 25.2  Non-trailer HEADERS after final response (RFC 9113 §8.1)
// ---------------------------------------------------------------------------

TEST_CASE("25.2 Second HEADERS without END_STREAM after final response", "[rfc_coverage]") {
  // RFC 9113 §8.1: "An endpoint that receives a HEADERS frame without the
  // END_STREAM flag set after receiving a final (non-informational) status
  // code MUST treat the corresponding request or response as malformed."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // First: 200 OK (no END_STREAM — body follows)
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  // DATA (no END_STREAM)
  feed(proto, makeDataFrame(sid, "body", false));
  // Second HEADERS without END_STREAM — malformed (not a trailer)
  std::vector<HeaderField> extra = {{"x-extra", "bad"}};
  ByteBuffer encoded = enc.encode(extra);
  HeadersFrame tf(sid);
  tf.data = std::move(encoded);
  tf.header.payloadLength = tf.data.size();
  tf.header.setEndHeadersFlag();
  // Deliberately NOT setting END_STREAM → violates §8.1
  auto result = feed(proto, tf.toBytes());
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

// ---------------------------------------------------------------------------
// 25.3  Trailer via HEADERS + CONTINUATION (RFC 9113 §8.1)
// ---------------------------------------------------------------------------

TEST_CASE("25.3 Trailer HEADERS with CONTINUATION", "[rfc_coverage]") {
  // RFC 9113 §8.1: trailer section is "one HEADERS frame (followed by zero
  // or more CONTINUATION frames)". HEADERS has END_STREAM but NOT END_HEADERS.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "body", false));

  // Trailer headers split across HEADERS + CONTINUATION
  std::vector<HeaderField> trailers = {{"grpc-status", "0"}, {"grpc-message", "OK"}};
  ByteBuffer encoded = enc.encode(trailers);
  std::uint32_t half = encoded.size() / 2;
  ByteBuffer part1 = ByteBuffer::from(encoded.begin(), encoded.begin() + half);
  ByteBuffer part2 = ByteBuffer::from(encoded.begin() + half, encoded.end());

  // HEADERS with END_STREAM but NOT END_HEADERS
  ByteBuffer hdr = makeResponseHeadersRaw(part1, sid, true, false);
  feed(proto, hdr);

  // CONTINUATION with END_HEADERS
  auto result = feed(proto, makeContinuation(sid, part2, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE_FALSE(completed.response.trailers.empty());
  bool foundGrpcStatus = false;
  for (auto& t : completed.response.trailers) {
    if (t.name == "grpc-status" && t.value == "0") {
      foundGrpcStatus = true;
    }
  }
  REQUIRE(foundGrpcStatus);
}

// ---------------------------------------------------------------------------
// 25.4  HPACK consistency after malformed response rejection
// ---------------------------------------------------------------------------

TEST_CASE("25.4 HPACK state preserved after malformed response", "[rfc_coverage]") {
  // When HEADERS decodes valid HPACK but validation fails (e.g., missing
  // :status), the HPACK dynamic table has already been updated. Since
  // malformed responses are stream errors (not connection errors), the
  // connection survives and subsequent streams must decode correctly.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid1 = submitSimpleGet(proto);

  // Valid HPACK encoding but missing :status pseudo-header → malformed
  std::vector<HeaderField> headers = {{"content-type", "text/plain"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid1, true);
  auto result = feed(proto, raw);
  // Should be stream error PROTOCOL_ERROR (malformed), NOT connection error
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid1, proto);

  // Connection is alive — open a new stream and verify HPACK decodes correctly
  StreamId sid2 = submitSimpleGet(proto, "/second");
  auto r2 = feed(proto, makeResponseHeaders(enc, sid2, 200, {{"x-test", "ok"}}, true));
  REQUIRE(hasEvent(r2, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(r2, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getStatusCode() == 200);
}

// ---------------------------------------------------------------------------
// 25.5  SETTINGS_INITIAL_WINDOW_SIZE does NOT change connection window
// ---------------------------------------------------------------------------

TEST_CASE("25.5 INITIAL_WINDOW_SIZE change does not affect connection window", "[rfc_coverage]") {
  // RFC 9113 §6.9.2: "A SETTINGS frame cannot alter the connection
  // flow-control window."
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100000}});

  // Submit POST with body of 70000 bytes.
  // If INITIAL_WINDOW_SIZE changed the connection window to 100000,
  // all 70000 bytes would be sent. But connection window stays at 65535.
  auto result = proto.submitRequest(makePostRequest(std::string(70000, 'X')));
  REQUIRE(result.hasSubmittedStream);

  auto frames = decodeOutbound(result);
  std::uint32_t dataBytes = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::DATA) {
      dataBytes += f.header.payloadLength;
    }
  }
  // Connection window (65535) is the bottleneck, not stream window (100000)
  REQUIRE(dataBytes == 65535);
}

// ---------------------------------------------------------------------------
// 25.6  DATA pad_length == remaining payload → zero data bytes → error
// ---------------------------------------------------------------------------

TEST_CASE("25.6 DATA with pad_length equal to remaining payload", "[rfc_coverage]") {
  // RFC 9113 §6.1: "If the length of the padding is the length of the frame
  // payload or greater, the recipient MUST treat this as a connection error
  // of type PROTOCOL_ERROR."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // Build: PADDED flag, payload = 0x02 (pad_length) + "AB" (2 bytes)
  // pad_length(2) == remaining(2) → all is padding, 0 data bytes → error
  std::string payload;
  payload += '\x02';  // pad_length = 2
  payload += "AB";    // 2 bytes — exactly pad_length
  ByteBuffer raw = makeRawFrame(FrameType::DATA, static_cast<uint8_t>(FrameFlag::PADDED), sid, payload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

// ---------------------------------------------------------------------------
// 25.7  DATA with PADDED flag and pad_length=0 (valid, 1-byte overhead)
// ---------------------------------------------------------------------------

TEST_CASE("25.7 DATA with PADDED flag and pad_length=0", "[rfc_coverage]") {
  // RFC 9113 §6.1 note: "A frame can be increased in size by one octet by
  // including a Pad Length field with a value of zero."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // PADDED flag, pad_length=0, data="hello"
  std::string payload;
  payload += '\x00';   // pad_length = 0
  payload += "hello";  // 5 data bytes, 0 padding bytes
  ByteBuffer raw = makeRawFrame(FrameType::DATA,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::PADDED) |
                                              static_cast<uint8_t>(FrameFlag::END_STREAM)),
                           sid,
                           payload);
  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.body == "hello");
}

// ---------------------------------------------------------------------------
// 25.8  GOAWAY: DATA on stream > lastStreamId still counted for flow control
// ---------------------------------------------------------------------------

TEST_CASE("25.8 DATA after GOAWAY on higher stream still counts for flow control",
          "[rfc_coverage]") {
  // RFC 9113 §6.8: "DATA frames MUST be counted toward the connection
  // flow-control window" even for streams beyond lastStreamId.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // Server responds on s1 with headers (no END_STREAM) then sends GOAWAY(last=1)
  feed(proto, makeResponseHeaders(enc, s1, 200, {}, false));
  feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));
  // s3 is failed, s1 survives

  // Server sends DATA on the failed s3 — implementation must account for
  // connection flow control (consume + refund), not crash.
  // Use > INFLOW_MIN_REFRESH (4096) to ensure WINDOW_UPDATE is emitted.
  auto result = feed(proto, makeDataFrame(s3, std::string(5000, 'X'), false));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));

  // Connection WINDOW_UPDATE should be emitted to refund the bytes
  auto frames = decodeOutbound(result);
  bool foundConnWu = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::WINDOW_UPDATE && f.header.streamId == 0) {
      foundConnWu = true;
    }
  }
  REQUIRE(foundConnWu);

  // s1 can still complete
  auto r2 = feed(proto, makeDataFrame(s1, "ok", true));
  REQUIRE(hasEvent(r2, ProtocolEventType::RESPONSE_COMPLETED));
  (void) s3;
}

// ---------------------------------------------------------------------------
// 25.9  HEADERS on different stream interrupts CONTINUATION sequence
// ---------------------------------------------------------------------------

TEST_CASE("25.9 HEADERS on different stream during CONTINUATION", "[rfc_coverage]") {
  // RFC 9113 §4.3: "field blocks MUST be transmitted as a contiguous sequence
  // of frames, with no interleaved frames of any other type or from any other
  // stream."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // HEADERS on s1 without END_HEADERS
  feed(proto, makeResponseHeaders(enc, s1, 200, {}, false, false));

  // HEADERS on s3 — different stream → PROTOCOL_ERROR
  auto result = feed(proto, makeResponseHeaders(enc, s3, 200, {}, true));
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

// ---------------------------------------------------------------------------
// 25.10 SETTINGS during CONTINUATION sequence
// ---------------------------------------------------------------------------

// Tests 25.10-25.12 removed: now covered by table-driven test 9.6

// ---------------------------------------------------------------------------
// 25.13 RST_STREAM with unknown error code
// ---------------------------------------------------------------------------

TEST_CASE("25.13 RST_STREAM with unknown error code", "[rfc_coverage]") {
  // RFC 9113 §7: "Unknown or unsupported error codes MUST NOT trigger any
  // special behavior."
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  auto result = feed(proto, makeRstStream(sid, static_cast<ErrorCode>(0x100)));
  REQUIRE(hasEvent(result, ProtocolEventType::STREAM_RESET_RECEIVED));
  auto evt = findEvent(result, ProtocolEventType::STREAM_RESET_RECEIVED);
  REQUIRE(evt.errorCode == static_cast<ErrorCode>(0x100));
  REQUIRE(proto.activeStreamCount() == 0);
  REQUIRE_FALSE(proto.isClosed());
}

// ---------------------------------------------------------------------------
// 25.14 GOAWAY with unknown error code
// ---------------------------------------------------------------------------

TEST_CASE("25.14 GOAWAY with unknown error code", "[rfc_coverage]") {
  Http2Protocol proto;
  handshake(proto);
  submitSimpleGet(proto, "/1");

  auto result = feed(proto, makeGoAway(0, static_cast<ErrorCode>(0xFF)));
  REQUIRE(hasEvent(result, ProtocolEventType::GOAWAY_RECEIVED));
  auto evt = findEvent(result, ProtocolEventType::GOAWAY_RECEIVED);
  REQUIRE(evt.errorCode == static_cast<ErrorCode>(0xFF));
  REQUIRE_FALSE(proto.isClosed());
}

// ---------------------------------------------------------------------------
// 25.15 HEADERS with PRIORITY flag but too-short payload
// ---------------------------------------------------------------------------

TEST_CASE("25.15 HEADERS with PRIORITY flag but insufficient payload", "[rfc_coverage]") {
  // RFC 9113 §6.2: when PRIORITY flag is set, 5 extra bytes
  // (Exclusive + Stream Dependency + Weight) are expected. If payload is
  // shorter than 5 bytes, the frame is too small to contain mandatory data.
  // RFC 9113 §4.2: "An endpoint MUST send an error code of FRAME_SIZE_ERROR
  // if a frame ... is too small to contain mandatory frame data."
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Build raw HEADERS with PRIORITY flag but only 3 bytes of payload
  std::string shortPayload(3, '\0');
  ByteBuffer raw = makeRawFrame(FrameType::HEADERS,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::PRIORITY) |
                                              static_cast<uint8_t>(FrameFlag::END_HEADERS) |
                                              static_cast<uint8_t>(FrameFlag::END_STREAM)),
                           sid,
                           shortPayload);
  auto result = feed(proto, raw);
  // HEADERS carries a field block → frame size error is a connection error.
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

// ---------------------------------------------------------------------------
// 25.16 Header name containing colon (non-pseudo)
// ---------------------------------------------------------------------------

TEST_CASE("25.16 Header name with colon in non-pseudo field", "[rfc_coverage]") {
  // RFC 9113 §8.2.1: "field names MUST NOT include a colon (ASCII COLON,
  // 0x3a)" except for pseudo-header fields.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"x:bad", "value"}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

// ---------------------------------------------------------------------------
// 25.17 :status boundary values
// ---------------------------------------------------------------------------

TEST_CASE("25.17.1 :status outside 100-599 rejected", "[rfc_coverage]") {
  // RFC 9110 §15: valid status codes are 100-599 inclusive.
  for (const char* bad : {"000", "099", "600", "999"}) {
    Http2Protocol proto;
    Hpack enc;
    handshake(proto);
    StreamId sid = submitSimpleGet(proto);

    std::vector<HeaderField> headers = {{":status", bad}};
    ByteBuffer encoded = enc.encode(headers);
    ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
    auto result = feed(proto, raw);
    requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
  }
}

TEST_CASE("25.17.2 :status boundary values 100 and 599 accepted", "[rfc_coverage]") {
  for (int code : {100, 599}) {
    Http2Protocol proto;
    Hpack enc;
    handshake(proto);
    StreamId sid = submitSimpleGet(proto);

    auto result = feed(proto, makeResponseHeaders(enc, sid, code, {}, code != 100));
    REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
    REQUIRE(hasEvent(result, ProtocolEventType::STREAM_HEADERS_RECEIVED));
  }
}

// ---------------------------------------------------------------------------
// 25.18 ENABLE_PUSH=0 sent in initial SETTINGS
// ---------------------------------------------------------------------------

TEST_CASE("25.18 Client sends ENABLE_PUSH=0 in initial SETTINGS", "[impl_behavior]") {
  // RFC 9113 §6.5.2: client should send ENABLE_PUSH=0 to disable push.
  Http2Protocol proto;
  auto result = proto.start();

  FrameCodec codec;
  codec.feed(result.outbound[1]);
  RawFrame raw;
  REQUIRE(codec.tryPop(&raw));
  SettingFrame sf(raw);

  bool foundEnablePush = false;
  for (auto& s : sf.settings) {
    if (s.first == static_cast<uint16_t>(Http2SettingParameter::ENABLE_PUSH)) {
      REQUIRE(s.second == 0);
      foundEnablePush = true;
    }
  }
  REQUIRE(foundEnablePush);
}

// ---------------------------------------------------------------------------
// 25.19 Multi-frame receiveBytes with mid-stream connection error
// ---------------------------------------------------------------------------

TEST_CASE("25.19 Connection error mid-batch stops processing", "[rfc_coverage]") {
  // When a single receiveBytes call contains multiple frames and one triggers
  // a connection error, subsequent frames must not be processed.
  Http2Protocol proto;
  handshake(proto);

  // Concatenate: valid PING + invalid SETTINGS (non-zero stream) + valid PING
  ByteBuffer ping1 = makePing("aaaaaaaa");
  ByteBuffer badSettings = makeRawFrame(FrameType::SETTINGS, 0, 1, std::string());
  ByteBuffer ping2 = makePing("bbbbbbbb");

  ByteBuffer combined(ping1.size() + badSettings.size() + ping2.size());
  combined.append(ping1.begin(), ping1.end());
  combined.append(badSettings.begin(), badSettings.end());
  combined.append(ping2.begin(), ping2.end());

  auto result = feed(proto, combined);

  // Connection error from bad SETTINGS
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(proto.isClosed());

  // First PING should have been processed (ACK emitted)
  REQUIRE(hasEvent(result, ProtocolEventType::PING_RECEIVED));

  // Second PING should NOT have been processed
  REQUIRE(countEvents(result, ProtocolEventType::PING_RECEIVED) == 1);
}

// ---------------------------------------------------------------------------
// 25.20 GOAWAY lastStreamId validation on outbound GOAWAY
// ---------------------------------------------------------------------------

// Test 25.20 removed: strict subset of test 1.2.1 (close emits GOAWAY with lastStreamId=0)

// ---------------------------------------------------------------------------
// 25.21 Connection error GOAWAY has correct lastStreamId=0
// ---------------------------------------------------------------------------

TEST_CASE("25.21 Connection error GOAWAY lastStreamId", "[rfc_coverage]") {
  Http2Protocol proto;
  handshake(proto);
  submitSimpleGet(proto);

  // Trigger connection error
  ByteBuffer raw = makeRawFrame(FrameType::SETTINGS, 0, 1, std::string());
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto, /*expectedLastStreamId=*/0);
}

// ---------------------------------------------------------------------------
// 25.22 Multiple streams: connection error fails ALL active streams
// ---------------------------------------------------------------------------

TEST_CASE("25.22 Connection error fails all active streams with correct error code",
          "[rfc_coverage]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");
  StreamId s5 = submitSimpleGet(proto, "/3");
  REQUIRE(proto.activeStreamCount() == 3);

  // Trigger connection error
  ByteBuffer raw = makeRawFrame(FrameType::SETTINGS, 0, 1, std::string());
  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));

  auto fails = findEvents(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails.size() == 3);
  // All streams should be failed
  std::vector<StreamId> failedIds;
  failedIds.reserve(fails.size());
  for (auto& f : fails) {
    failedIds.push_back(f.streamId);
  }
  REQUIRE(std::find(failedIds.begin(), failedIds.end(), s1) != failedIds.end());
  REQUIRE(std::find(failedIds.begin(), failedIds.end(), s3) != failedIds.end());
  REQUIRE(std::find(failedIds.begin(), failedIds.end(), s5) != failedIds.end());
  REQUIRE(proto.activeStreamCount() == 0);
}

// ---------------------------------------------------------------------------
// 25.23 Empty SETTINGS frame is valid
// ---------------------------------------------------------------------------

TEST_CASE("25.23 Empty SETTINGS frame (zero settings) is valid", "[rfc_coverage]") {
  Http2Protocol proto;
  proto.start();

  auto result = feed(proto, makeEmptyServerSettings());
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

// ---------------------------------------------------------------------------
// 25.24 Unknown setting mixed with valid settings in one frame
// ---------------------------------------------------------------------------

TEST_CASE("25.24 Unknown setting ignored while valid settings applied", "[rfc_coverage]") {
  // RFC 9113 §6.5.2: "An endpoint that receives a SETTINGS frame with any
  // unknown or unsupported identifier MUST ignore that setting."
  Http2Protocol proto;
  proto.start();

  ByteBuffer settings = makeServerSettings(
      {{0xFF, 42},  // unknown — must be ignored
       {static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 3}});
  auto result = feed(proto, settings);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));

  // Verify MAX_CONCURRENT_STREAMS=3 was applied
  feed(proto, makeSettingsAck());
  auto r1 = proto.submitRequest(makeGetRequest("/1"));
  auto r2 = proto.submitRequest(makeGetRequest("/2"));
  auto r3 = proto.submitRequest(makeGetRequest("/3"));
  auto r4 = proto.submitRequest(makeGetRequest("/4"));
  REQUIRE(r1.hasSubmittedStream);
  REQUIRE(r2.hasSubmittedStream);
  REQUIRE(r3.hasSubmittedStream);
  REQUIRE_FALSE(r4.hasSubmittedStream);
}

// ---------------------------------------------------------------------------
// 25.25 DATA on closed stream counted for connection flow control with exact refund
// ---------------------------------------------------------------------------

TEST_CASE("25.25 DATA on closed stream refunds exact connection window amount", "[rfc_coverage]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  completeResponse(proto, enc, sid);
  REQUIRE(proto.activeStreamCount() == 0);

  // Feed DATA on closed stream — must be counted and refunded.
  // Use > INFLOW_MIN_REFRESH (4096) to ensure WINDOW_UPDATE is emitted.
  const std::uint32_t dataSize = 5000;
  auto result = feed(proto, makeDataFrame(sid, std::string(dataSize, 'X'), false));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));

  auto frames = decodeOutbound(result);
  std::uint32_t totalRefund = 0;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::WINDOW_UPDATE && f.header.streamId == 0) {
      WindowUpdateFrame wu(f);
      totalRefund += wu.windowSizeIncrement;
    }
  }
  // Refund must exactly equal the consumed bytes
  REQUIRE(totalRefund == dataSize);
}

// Test 25.26 removed: now covered by table-driven test 9.6

// ---------------------------------------------------------------------------
// 25.27 GOAWAY with ENHANCE_YOUR_CALM from server
// ---------------------------------------------------------------------------

TEST_CASE("25.27 GOAWAY with ENHANCE_YOUR_CALM error code", "[rfc_coverage]") {
  Http2Protocol proto;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");

  auto result = feed(proto, makeGoAway(0, ErrorCode::ENHANCE_YOUR_CALM, "too many requests"));
  REQUIRE(hasEvent(result, ProtocolEventType::GOAWAY_RECEIVED));
  auto evt = findEvent(result, ProtocolEventType::GOAWAY_RECEIVED);
  REQUIRE(evt.errorCode == ErrorCode::ENHANCE_YOUR_CALM);
  REQUIRE(evt.debugData == "too many requests");

  auto fails = findEvents(result, ProtocolEventType::RESPONSE_FAILED);
  REQUIRE(fails.size() == 1);
  REQUIRE(fails[0].streamId == s1);
}

// Test 25.28 removed: exact duplicate of test 14.4.4

// ---------------------------------------------------------------------------
// 25.29 Reserved bit in stream identifier is masked
// ---------------------------------------------------------------------------

TEST_CASE("25.29 Reserved bit in frame stream identifier masked", "[rfc_coverage]") {
  // RFC 9113 §4.1: "Reserved: ... MUST remain unset when sending and MUST be
  // ignored when receiving." The frame header streamId field has bit 31
  // reserved. FrameHeader::decode masks it: streamId &= 0x7FFFFFFF.
  Http2Protocol proto;
  handshake(proto);

  // Build raw PING with streamId = 0x80000000 (reserved bit set, actual ID=0)
  ByteBuffer payload = ByteBuffer(8u);
  for (int i = 0; i < 8; ++i) {
    payload.append<uint8_t>('X');
  }

  FrameHeader hdr;
  hdr.type = FrameType::PING;
  hdr.flags = 0;
  hdr.streamId = 0x80000000u;  // reserved bit set
  hdr.payloadLength = 8;
  ByteBuffer headerBytes = FrameHeader::encode(hdr);
  ByteBuffer raw = ByteBuffer(17u);
  raw.append(headerBytes.begin(), headerBytes.end());
  raw.append(payload.begin(), payload.end());

  auto result = feed(proto, raw);
  // Reserved bit masked → streamId=0 → valid PING → ACK
  REQUIRE(hasEvent(result, ProtocolEventType::PING_RECEIVED));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));

  // Verify PING ACK was sent with correct opaque data
  auto frames = decodeOutbound(result);
  bool foundAck = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::PING && f.header.hasAckFlag()) {
      PingFrame pf(f);
      std::string ackPayload(pf.data.begin(), pf.data.end());
      REQUIRE(ackPayload == "XXXXXXXX");
      foundAck = true;
    }
  }
  REQUIRE(foundAck);
}

// ---------------------------------------------------------------------------
// 25.30 Content-Length mismatch caught before END_STREAM (excess detected mid-stream)
// ---------------------------------------------------------------------------

TEST_CASE("25.30 Content-Length exceeded across multiple DATA before END_STREAM",
          "[impl_behavior]") {
  // Content-Length=3, send 2 bytes then 2 more (total 4 > 3) WITHOUT END_STREAM.
  // The current implementation checks content-length only at END_STREAM,
  // so mid-stream excess is not caught until END_STREAM arrives.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {{"content-length", "3"}}, false));
  // First DATA: 2 bytes — within budget
  auto r1 = feed(proto, makeDataFrame(sid, "AB", false));
  REQUIRE_FALSE(hasEvent(r1, ProtocolEventType::RESPONSE_FAILED));

  // Second DATA: 2 more bytes — total now 4, exceeds 3.
  // Implementation defers content-length validation to END_STREAM.
  auto r2 = feed(proto, makeDataFrame(sid, "CD", false));
  REQUIRE_FALSE(hasEvent(r2, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE_FALSE(proto.isClosed());

  // END_STREAM triggers the content-length check
  auto r3 = feed(proto, makeDataFrame(sid, "", true));
  auto frames = decodeOutbound(r3);
  bool foundRst = false;
  for (auto& f : frames) {
    if (f.header.getType() == FrameType::RST_STREAM && f.header.streamId == sid) {
      foundRst = true;
    }
  }
  REQUIRE(foundRst);
  REQUIRE(hasEvent(r3, ProtocolEventType::RESPONSE_FAILED));
}

// =============================================================================
// Section 26: Missing RFC Scenarios [rfc_missing]
// =============================================================================

// ---------------------------------------------------------------------------
// 26.1  CONNECT with :scheme/:path (RFC 9113 §8.5)
// ---------------------------------------------------------------------------

TEST_CASE("26.1 CONNECT pseudo-header violations rejected", "[rfc_missing]") {
  // RFC 9113 §8.5: CONNECT MUST NOT include :scheme/:path, MUST include :authority.
  struct Case {
    const char* label;
    std::map<std::string, std::string> headers;
  };
  Case cases[] = {
      {"CONNECT with :scheme",
       {{":method", "CONNECT"}, {":authority", "example.com:443"}, {":scheme", "https"}}},
      {"CONNECT with :path",
       {{":method", "CONNECT"}, {":authority", "example.com:443"}, {":path", "/"}}},
      {"CONNECT without :authority", {{":method", "CONNECT"}}},
  };
  for (auto& c : cases) {
    Http2Protocol proto;
    handshake(proto);
    auto nextBefore = proto.nextLocalStreamId();

    Request req;
    req.headers = c.headers;
    auto result = proto.submitRequest(req);
    REQUIRE_FALSE(result.hasSubmittedStream);
    REQUIRE(result.outbound.empty());
    auto fail = findEvent(result, ProtocolEventType::RESPONSE_FAILED);
    REQUIRE(fail.errorCode == ErrorCode::PROTOCOL_ERROR);
    // Stream id must not advance
    REQUIRE(proto.nextLocalStreamId() == nextBefore);
  }
}

// Test 26.2.2 removed: duplicate of test 12.1.8 (valid CONNECT request)

// ---------------------------------------------------------------------------
// 26.3  GOAWAY: HEADERS on higher stream still HPACK-decoded (RFC §6.8)
// ---------------------------------------------------------------------------

TEST_CASE("26.3 GOAWAY then HEADERS on higher stream keeps HPACK in sync", "[rfc_missing]") {
  // RFC 9113 §6.8: "HEADERS, PUSH_PROMISE, and CONTINUATION frames MUST
  // be minimally processed to ensure that the state maintained for field
  // section compression is consistent."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  // GOAWAY with lastStreamId=s1 → s3 is failed
  feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));
  REQUIRE(proto.activeStreamCount() == 1);

  // Server sends HEADERS on the failed s3 with dynamic table entries.
  // Even though s3 is failed, the HPACK block must be decoded.
  feed(proto, makeResponseHeaders(enc, s3, 200, {{"x-goaway-hpack", "sync-test"}}, true));

  // s1 must still decode correctly using the same HPACK state
  auto result = feed(proto, makeResponseHeaders(enc, s1, 200, {{"x-after-goaway", "ok"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getHeader("x-after-goaway") == "ok");
}

TEST_CASE("26.4 GOAWAY then HEADERS+CONTINUATION on higher stream keeps HPACK in sync",
          "[rfc_missing]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));

  // HEADERS without END_HEADERS on failed s3 → ghost path
  std::vector<HeaderField> headers = {{":status", "200"}, {"x-ghost-cont", "hpack-sync"}};
  ByteBuffer encoded = enc.encode(headers);
  std::uint32_t half = encoded.size() / 2;
  ByteBuffer part1 = ByteBuffer::from(encoded.begin(), encoded.begin() + half);
  ByteBuffer part2 = ByteBuffer::from(encoded.begin() + half, encoded.end());

  feed(proto, makeResponseHeadersRaw(part1, s3, false, false));
  feed(proto, makeContinuation(s3, part2, true));
  REQUIRE_FALSE(proto.isClosed());

  // s1 must decode correctly
  auto result = feed(proto, makeResponseHeaders(enc, s1, 200, {{"x-ok", "true"}}, true));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

TEST_CASE("26.4.2 GOAWAY then malformed HPACK on discarded stream → COMPRESSION_ERROR",
          "[rfc_missing]") {
  // RFC 9113 §4.3/§6.8: HEADERS on discarded streams MUST still be decoded.
  // A decode failure MUST trigger COMPRESSION_ERROR (connection error).
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));
  REQUIRE(proto.activeStreamCount() == 1);

  // Feed invalid HPACK block on discarded stream s3
  ByteBuffer garbage = ByteBuffer(4u);
  garbage.append<uint8_t>(0xFF);
  garbage.append<uint8_t>(0xFF);
  garbage.append<uint8_t>(0xFF);
  garbage.append<uint8_t>(0xFF);
  ByteBuffer raw = makeResponseHeadersRaw(garbage, s3, true);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::COMPRESSION_ERROR, proto);
}

TEST_CASE("26.4.3 GOAWAY then malformed CONTINUATION on discarded stream → COMPRESSION_ERROR",
          "[rfc_missing]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId s1 = submitSimpleGet(proto, "/1");
  StreamId s3 = submitSimpleGet(proto, "/2");

  feed(proto, makeGoAway(s1, ErrorCode::NO_ERROR));

  // Valid HEADERS start (without END_HEADERS) on discarded s3
  std::vector<HeaderField> headers = {{":status", "200"}};
  ByteBuffer encoded = enc.encode(headers);
  feed(proto, makeResponseHeadersRaw(encoded, s3, false, false));

  // Malformed CONTINUATION with garbage HPACK
  ByteBuffer garbage = ByteBuffer(4u);
  garbage.append<uint8_t>(0xFF);
  garbage.append<uint8_t>(0xFF);
  garbage.append<uint8_t>(0xFF);
  garbage.append<uint8_t>(0xFF);
  auto result = feed(proto, makeContinuation(s3, garbage, true));
  requireConnectionError(result, ErrorCode::COMPRESSION_ERROR, proto);
}

// ---------------------------------------------------------------------------
// 26.5  RST_STREAM(NO_ERROR) after complete response must not discard it
// ---------------------------------------------------------------------------

TEST_CASE("26.5 RST_STREAM(NO_ERROR) after complete response preserves data", "[rfc_missing]") {
  // RFC 9113 §8.7 / §5.4.2: "A server MAY request that the client abort
  // transmission of a request without error by sending a RST_STREAM with
  // an error code of NO_ERROR after sending a complete response."
  // The client must NOT discard the already-received response.
  Http2Protocol proto;
  Hpack enc;
  // Small window to keep body pending so stream stays in map
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 2}});

  auto submitResult = proto.submitRequest(makePostRequest("HelloWorld"));
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  // Server sends complete response (200 + END_STREAM)
  auto hdrResult = feed(proto, makeResponseHeaders(enc, sid, 200, {{"x-resp", "ok"}}, true));
  // Response must be delivered
  REQUIRE(hasEvent(hdrResult, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(hdrResult, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getStatusCode() == 200);
  REQUIRE(completed.response.getHeader("x-resp") == "ok");

  // Server sends RST_STREAM(NO_ERROR) to request client stop uploading
  auto rstResult = feed(proto, makeRstStream(sid, ErrorCode::NO_ERROR));
  // Must not generate a connection error or discard the already-delivered response
  REQUIRE_FALSE(hasEvent(rstResult, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE_FALSE(proto.isClosed());
}

// ---------------------------------------------------------------------------
// 26.6  Zero-window: empty DATA + END_STREAM allowed (RFC §6.9)
// ---------------------------------------------------------------------------

TEST_CASE("26.6 Empty DATA+END_STREAM allowed when inbound window is zero", "[rfc_missing]") {
  // RFC 9113 §6.9: "Frames with zero length with the END_STREAM flag set
  // (that is, an empty DATA frame) MAY be sent if there is no available
  // space in either flow-control window."
  // Test receive side: server sends empty DATA+END_STREAM when client window=0.
  Http2Protocol proto;
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::INITIAL_WINDOW_SIZE), 100}});

  StreamId sid = submitSimpleGet(proto);
  Hpack enc;
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // Consume the entire 100-byte stream window with DATA (no END_STREAM)
  feed(proto, makeDataFrame(sid, std::string(100, 'X'), false));

  // Now server sends empty DATA + END_STREAM — must not trigger flow control error
  auto result = feed(proto, makeDataFrame(sid, "", true));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

// ---------------------------------------------------------------------------
// 26.7  HEADERS with valid padding boundary (pad_length=0)
// ---------------------------------------------------------------------------

TEST_CASE("26.7 HEADERS with PADDED flag and pad_length=0 is valid", "[rfc_missing]") {
  // RFC 9113 §6.2 note: "A frame can be increased in size by one octet by
  // including a Pad Length field with a value of zero."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"x-padded", "zero"}};
  ByteBuffer encoded = enc.encode(headers);

  HeadersFrame frame(sid);
  frame.header.setPaddedFlag();
  frame.header.setEndHeadersFlag();
  frame.header.setEndStreamFlag();
  frame.padLength = 0;
  frame.data = ByteBuffer::from(encoded.begin(), encoded.end());
  // payloadLength = 1 (pad_length byte) + data size + 0 (padding)
  frame.header.payloadLength = 1 + frame.data.size();
  ByteBuffer raw = frame.toBytes();

  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getStatusCode() == 200);
  REQUIRE(completed.response.getHeader("x-padded") == "zero");
}

// ---------------------------------------------------------------------------
// 26.8  HEADERS with valid padding (pad_length < remaining)
// ---------------------------------------------------------------------------

TEST_CASE("26.8 HEADERS with valid PADDED and small padding", "[rfc_missing]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  std::vector<HeaderField> headers = {{":status", "200"}, {"x-padded", "small"}};
  ByteBuffer encoded = enc.encode(headers);

  HeadersFrame frame(sid);
  frame.header.setPaddedFlag();
  frame.header.setEndHeadersFlag();
  frame.header.setEndStreamFlag();
  frame.padLength = 3;
  frame.data = ByteBuffer::from(encoded.begin(), encoded.end());
  frame.padding = ByteBuffer(3u);
  for (int i = 0; i < 3; ++i) {
    frame.padding.append<uint8_t>(0);
  }
  // payloadLength = 1 (pad_length) + data + 3 (padding)
  frame.header.payloadLength = 1 + frame.data.size() + 3;
  ByteBuffer raw = frame.toBytes();

  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
  auto completed = findEvent(result, ProtocolEventType::RESPONSE_COMPLETED);
  REQUIRE(completed.response.getHeader("x-padded") == "small");
}

// ---------------------------------------------------------------------------
// 26.9  HEADERS padding boundary: pad_length == remaining → error
// ---------------------------------------------------------------------------

TEST_CASE("26.9 HEADERS with pad_length equal to remaining payload", "[rfc_missing]") {
  // RFC 9113 §6.2: "If the length of the padding is the length of the frame
  // payload or greater, the recipient MUST treat this as a connection error
  // of type PROTOCOL_ERROR."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // Build PADDED HEADERS where pad_length consumes all remaining space.
  // payload = pad_length_byte(1) + hpack_data(N). Set pad_length = N.
  std::vector<HeaderField> headers = {{":status", "200"}};
  ByteBuffer encoded = enc.encode(headers);
  std::uint32_t hpackSize = encoded.size();

  // Construct raw frame: [pad_length=hpackSize] [hpackSize bytes data]
  // remaining = payloadLength - 1 = hpackSize, pad_length = hpackSize → pad >= remaining
  std::string payload;
  payload += static_cast<char>(static_cast<uint8_t>(hpackSize));
  payload.append(encoded.begin(), encoded.end());
  ByteBuffer raw = makeRawFrame(FrameType::HEADERS,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::PADDED) |
                                              static_cast<uint8_t>(FrameFlag::END_HEADERS) |
                                              static_cast<uint8_t>(FrameFlag::END_STREAM)),
                           sid,
                           payload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::PROTOCOL_ERROR, proto);
}

// ---------------------------------------------------------------------------
// 26.10  SETTINGS: duplicate setting id → last value wins
// ---------------------------------------------------------------------------

TEST_CASE("26.10 SETTINGS with duplicate setting id uses last value", "[rfc_missing]") {
  // RFC 9113 §6.5.3: "The values in the SETTINGS frame MUST be processed
  // in the order they appear." Duplicate keys → last value wins.
  Http2Protocol proto;
  proto.start();

  // Send MAX_CONCURRENT_STREAMS twice: first 10, then 1.
  ByteBuffer settings = makeServerSettings(
      {{static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 10},
       {static_cast<uint16_t>(Http2SettingParameter::MAX_CONCURRENT_STREAMS), 1}});
  auto result = feed(proto, settings);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::SETTINGS_RECEIVED));

  // Verify last value (1) was applied
  feed(proto, makeSettingsAck());
  auto r1 = proto.submitRequest(makeGetRequest("/1"));
  auto r2 = proto.submitRequest(makeGetRequest("/2"));
  REQUIRE(r1.hasSubmittedStream);
  REQUIRE_FALSE(r2.hasSubmittedStream);  // blocked by MAX_CONCURRENT_STREAMS=1
}

// ---------------------------------------------------------------------------
// 26.11  MAX_HEADER_LIST_SIZE: decompressed size exceeds limit
// ---------------------------------------------------------------------------

TEST_CASE("26.11 Decompressed header list size exceeds MAX_HEADER_LIST_SIZE", "[impl_behavior]") {
  // RFC 9113 §6.5.2: MAX_HEADER_LIST_SIZE is the max size of the uncompressed
  // header field section. headerListSize = sum(name.size + value.size + 32).
  Http2Protocol proto;
  // Set a small MAX_HEADER_LIST_SIZE from server
  handshake(proto, {{static_cast<uint16_t>(Http2SettingParameter::MAX_HEADER_LIST_SIZE), 100}});

  StreamId sid = submitSimpleGet(proto);
  Hpack enc;

  // :status=200 → 7+3+32=42, x-big=<60 chars> → 5+60+32=97. Total=139 > 100.
  std::vector<HeaderField> headers = {{":status", "200"}, {"x-big", std::string(60, 'A')}};
  ByteBuffer encoded = enc.encode(headers);
  ByteBuffer raw = makeResponseHeadersRaw(encoded, sid, true);
  auto result = feed(proto, raw);
  // Oversized header list is a malformed response → stream error
  requireStreamError(result, ErrorCode::ENHANCE_YOUR_CALM, sid, proto);
}

// ---------------------------------------------------------------------------
// 26.12  PRIORITY on idle stream is legal (RFC §5.1)
// ---------------------------------------------------------------------------

TEST_CASE("26.12 PRIORITY on idle stream accepted, parser aligned", "[rfc_missing]") {
  // RFC 9113 §5.1: PRIORITY is allowed on idle streams.
  // Verify parser alignment by following with a valid PING.
  Http2Protocol proto;
  handshake(proto);

  // stream 99 was never opened — it's idle
  ByteBuffer pri = makePriority(99, 0, 16);
  auto result = feed(proto, pri);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(result.events.empty());

  // Subsequent PING must parse correctly
  auto pingResult = feed(proto, makePing("IDLEPRI!"));
  REQUIRE(hasEvent(pingResult, ProtocolEventType::PING_RECEIVED));
  REQUIRE_FALSE(proto.isClosed());
}

// ---------------------------------------------------------------------------
// 26.13  Known frame with undefined flags → flags ignored
// ---------------------------------------------------------------------------

TEST_CASE("26.13 Known frame type with undefined flags ignored", "[rfc_missing]") {
  // RFC 9113 §4.1: "Flags that have no defined semantics for a particular
  // frame type MUST be ignored."
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // DATA frame with undefined flag bits set (0x02, 0x04, 0x10, 0x20, 0x40, 0x80).
  // Only END_STREAM(0x01) and PADDED(0x08) are defined for DATA.
  // Set END_STREAM + undefined bits 0x76.
  std::string payload = "hello";
  ByteBuffer raw = makeRawFrame(FrameType::DATA,
                           static_cast<Flags>(0x01 | 0x76),  // END_STREAM + undefined
                           sid,
                           payload);
  auto result = feed(proto, raw);
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

// ---------------------------------------------------------------------------
// 26.14  HEAD/204/304 responses with DATA body → malformed (RFC §8.1.1)
// ---------------------------------------------------------------------------

TEST_CASE("26.14 HEAD response with non-empty DATA rejected", "[rfc_missing]") {
  // RFC 9110 §9.3.2: server MUST NOT send content in HEAD response.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);

  auto submitResult = proto.submitRequest(makeHeadRequest());
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "unexpected-body", true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("26.15 204 response with non-empty DATA rejected", "[rfc_missing]") {
  // RFC 9110 §15.3.5: 204 response has no content.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 204, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "unexpected-body", true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("26.16 304 response with non-empty DATA rejected", "[rfc_missing]") {
  // RFC 9110 §15.4.5: 304 response has no content.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 304, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "unexpected-body", true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("26.17 HEAD response with empty DATA+END_STREAM accepted", "[rfc_missing]") {
  // Empty DATA+END_STREAM has no content — should be allowed.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);

  auto submitResult = proto.submitRequest(makeHeadRequest());
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  auto result = feed(proto, makeDataFrame(sid, "", true));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
  REQUIRE(hasEvent(result, ProtocolEventType::RESPONSE_COMPLETED));
}

// ---------------------------------------------------------------------------
// 26.18  HEADERS with PADDED+PRIORITY but payload < 6 bytes
// ---------------------------------------------------------------------------

TEST_CASE("26.18 HEADERS with PADDED+PRIORITY and insufficient payload", "[rfc_missing]") {
  // PADDED+PRIORITY requires at least 1 (pad_length) + 5 (priority) = 6 bytes.
  // A shorter payload must be FRAME_SIZE_ERROR.
  Http2Protocol proto;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // 5 bytes payload with both PADDED and PRIORITY flags
  std::string payload(5, '\0');
  ByteBuffer raw = makeRawFrame(FrameType::HEADERS,
                           static_cast<Flags>(static_cast<uint8_t>(FrameFlag::PADDED) |
                                              static_cast<uint8_t>(FrameFlag::PRIORITY) |
                                              static_cast<uint8_t>(FrameFlag::END_HEADERS)),
                           sid,
                           payload);
  auto result = feed(proto, raw);
  requireConnectionError(result, ErrorCode::FRAME_SIZE_ERROR, proto);
}

// ---------------------------------------------------------------------------
// 26.19  CONNECT tunnel: HEADERS after 2xx is stream error (RFC §8.5)
// ---------------------------------------------------------------------------

TEST_CASE("26.19 CONNECT tunnel rejects HEADERS after 2xx response", "[rfc_missing]") {
  // RFC 9113 §8.5: after a successful CONNECT response (2xx), the stream
  // becomes a tunnel. Only DATA is allowed; receiving HEADERS is a stream error.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);

  Request req;
  req.headers[":method"] = "CONNECT";
  req.headers[":authority"] = "example.com:443";
  auto submitResult = proto.submitRequest(req);
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  // Server sends 200 (successful CONNECT) without END_STREAM — tunnel open
  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // Server sends HEADERS (e.g. trailers) on the tunnel stream — must be rejected
  auto result = feed(proto, makeResponseHeaders(enc, sid, 200, {}, true));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("26.20 CONNECT tunnel allows DATA after 2xx", "[rfc_missing]") {
  // After successful CONNECT, DATA frames carry tunnel payload.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);

  Request req;
  req.headers[":method"] = "CONNECT";
  req.headers[":authority"] = "example.com:443";
  auto submitResult = proto.submitRequest(req);
  REQUIRE(submitResult.hasSubmittedStream);
  StreamId sid = submitResult.submittedStreamId;

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));

  // DATA on tunnel is fine
  auto result = feed(proto, makeDataFrame(sid, "tunnel-data", false));
  REQUIRE_FALSE(hasEvent(result, ProtocolEventType::CONNECTION_ERROR));
}

// ---------------------------------------------------------------------------
// 26.21  Known frame with undefined flags + follow-up proves parser alignment
// ---------------------------------------------------------------------------

TEST_CASE("26.21 PING with undefined flags still parsed, follow-up aligned", "[rfc_missing]") {
  // RFC 9113 §4.1: undefined flags MUST be ignored.
  // Verify parser alignment by following with another valid frame.
  Http2Protocol proto;
  handshake(proto);

  // PING with undefined flag bits (0x02, 0x04, etc.) — only ACK(0x01) is defined
  ByteBuffer raw = makeRawFrame(FrameType::PING,
                           static_cast<Flags>(0x76),  // undefined bits only
                           0,
                           std::string("FLAGTEST"));
  auto result = feed(proto, raw);
  REQUIRE(hasEvent(result, ProtocolEventType::PING_RECEIVED));

  // Follow with valid SETTINGS — must parse correctly
  auto settingsResult = feed(proto, makeEmptyServerSettings());
  REQUIRE(hasEvent(settingsResult, ProtocolEventType::SETTINGS_RECEIVED));
}

// ---------------------------------------------------------------------------
// 26.22  DATA after 1xx but before final response → malformed
// ---------------------------------------------------------------------------

TEST_CASE("26.22 DATA after 100 Continue but before final response", "[rfc_missing]") {
  // RFC 9113 §8.1: response = HEADERS(1xx)* + HEADERS(final) + DATA* + HEADERS(trailers)?
  // DATA arriving after 1xx but before the final HEADERS is malformed.
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  // 100 Continue (no END_STREAM)
  feed(proto, makeResponseHeaders(enc, sid, 100, {}, false));

  // DATA before final response HEADERS — malformed
  auto result = feed(proto, makeDataFrame(sid, "premature", false));
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

// ---------------------------------------------------------------------------
// 26.23  Trailer field validation (same rules as response headers)
// ---------------------------------------------------------------------------

TEST_CASE("26.23 Trailer with uppercase header name rejected", "[rfc_missing]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "body", false));

  // Trailer with uppercase name
  std::vector<HeaderField> trailers = {{"X-Upper", "bad"}};
  ByteBuffer encoded = enc.encode(trailers);
  HeadersFrame tf(sid);
  tf.data = std::move(encoded);
  tf.header.payloadLength = tf.data.size();
  tf.header.setEndHeadersFlag();
  tf.header.setEndStreamFlag();
  auto result = feed(proto, tf.toBytes());
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("26.24 Trailer with connection-specific header rejected", "[rfc_missing]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "body", false));

  // Trailer with connection-specific header
  std::vector<HeaderField> trailers = {{"transfer-encoding", "chunked"}};
  ByteBuffer encoded = enc.encode(trailers);
  HeadersFrame tf(sid);
  tf.data = std::move(encoded);
  tf.header.payloadLength = tf.data.size();
  tf.header.setEndHeadersFlag();
  tf.header.setEndStreamFlag();
  auto result = feed(proto, tf.toBytes());
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}

TEST_CASE("26.25 Trailer with NUL in value rejected", "[rfc_missing]") {
  Http2Protocol proto;
  Hpack enc;
  handshake(proto);
  StreamId sid = submitSimpleGet(proto);

  feed(proto, makeResponseHeaders(enc, sid, 200, {}, false));
  feed(proto, makeDataFrame(sid, "body", false));

  // Trailer with NUL in value
  std::string badValue = "before";
  badValue += '\0';
  badValue += "after";
  std::vector<HeaderField> trailers = {{"x-trailer", badValue}};
  ByteBuffer encoded = enc.encode(trailers);
  HeadersFrame tf(sid);
  tf.data = std::move(encoded);
  tf.header.payloadLength = tf.data.size();
  tf.header.setEndHeadersFlag();
  tf.header.setEndStreamFlag();
  auto result = feed(proto, tf.toBytes());
  requireStreamError(result, ErrorCode::PROTOCOL_ERROR, sid, proto);
}
