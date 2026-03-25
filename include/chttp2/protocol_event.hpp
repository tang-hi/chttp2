#pragma once

#include <cstdint>
#include <string>

#include "chttp2/http2_constants.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

// ProtocolEvent is the semantic output consumed by Session.
//
// Coverage against the current frame model in frame.hpp/frame.cpp:
// - Explicitly covered: DATA, HEADERS, RST_STREAM, SETTINGS,
//   PUSH_PROMISE, PING, GOAWAY, WINDOW_UPDATE, unknown extension frames.
// - CONTINUATION is intentionally not exposed as its own event. It is part of
//   one header block and should be folded into HEADERS-related events after
//   validation and reassembly.
// - PRIORITY is parsed and validated by Protocol, but intentionally ignored.
// - Most locally-emitted frames are already represented in `ProtocolStepResult`
//   via `outbound`. They do not need duplicate sent-events unless the send
//   itself changes connection lifecycle semantics, which is why GOAWAY_SENT is
//   still modeled explicitly.

enum class ProtocolEventType : std::uint8_t {
  // Local client preface + initial SETTINGS have been emitted and protocol
  // state is now active.
  CONNECTION_STARTED,
  // Peer SETTINGS received. Verify applied values via Protocol query methods.
  SETTINGS_RECEIVED,
  // Peer acknowledged our SETTINGS.
  SETTINGS_ACK_RECEIVED,
  // Peer PING received. Uses `opaqueData` for the 8-byte payload.
  PING_RECEIVED,
  // Peer PING ACK received. Uses `opaqueData`.
  PING_ACK_RECEIVED,
  // Local GOAWAY emitted. Uses `lastStreamId`, `errorCode`, `debugData`.
  GOAWAY_SENT,
  // Peer GOAWAY received. Uses `lastStreamId`, `errorCode`, `debugData`.
  GOAWAY_RECEIVED,
  // Connection-level protocol failure. Uses `errorCode`.
  CONNECTION_ERROR,
  // Protocol state has fully transitioned to closed.
  CONNECTION_CLOSED,
  // Underlying transport died independently of protocol semantics.
  TRANSPORT_CLOSED,

  // One local stream object was created. Uses `streamId`.
  STREAM_OPENED,
  // One complete response header block has been assembled. CONTINUATION frames
  // are folded here. Uses `streamId`, `response`.
  STREAM_HEADERS_RECEIVED,
  // Peer sent RST_STREAM. Uses `streamId`, `errorCode`.
  STREAM_RESET_RECEIVED,
  // Stream state machine has fully closed. Uses `streamId`, `errorCode`.
  STREAM_CLOSED,

  // Peer changed a flow-control window. Uses `streamId`,
  // `window_size_increment`.
  WINDOW_UPDATE_RECEIVED,
  // Peer PUSH_PROMISE received. Uses `streamId`, `promisedStreamId`.
  PUSH_PROMISE_RECEIVED,
  // Unsupported or extension frame received. Uses `frameType`, `streamId`,
  // `flags`, and optionally `opaqueData`.
  UNKNOWN_FRAME_RECEIVED,

  // Convenience client-facing aggregate event. Uses `streamId`, `response`.
  RESPONSE_COMPLETED,
  // Convenience client-facing aggregate error. Uses `streamId`, `errorCode`.
  RESPONSE_FAILED
};

struct ProtocolEvent {
  // What semantic transition happened.
  ProtocolEventType type{};
  // Zero means connection-scoped; non-zero means stream-scoped.
  StreamId streamId{};
  // Current aggregated response snapshot for response-related events.
  Response response;
  // Generic protocol error/result code for GOAWAY, RST_STREAM, connection and
  // stream failure events.
  ErrorCode errorCode{};
  // GOAWAY debug data.
  std::string debugData;
  // Generic opaque payload, currently used for PING and UNKNOWN_FRAME.
  std::string opaqueData;
  // GOAWAY last processed stream id.
  StreamId lastStreamId{};
  // PUSH_PROMISE promised stream id.
  StreamId promisedStreamId{};
  // WINDOW_UPDATE increment.
  std::uint32_t windowSizeIncrement{};
  // UNKNOWN_FRAME payload.
  FrameType frameType{};
  Flags flags{};

  static ProtocolEvent connectionStarted() {
    ProtocolEvent event;
    event.type = ProtocolEventType::CONNECTION_STARTED;
    return event;
  }

  static ProtocolEvent settingsReceived() {
    ProtocolEvent event;
    event.type = ProtocolEventType::SETTINGS_RECEIVED;
    return event;
  }

  static ProtocolEvent settingsAckReceived() {
    ProtocolEvent event;
    event.type = ProtocolEventType::SETTINGS_ACK_RECEIVED;
    return event;
  }

  static ProtocolEvent pingReceived(const std::string& opaqueData) {
    ProtocolEvent event;
    event.type = ProtocolEventType::PING_RECEIVED;
    event.opaqueData = opaqueData;
    return event;
  }

  static ProtocolEvent pingAckReceived(const std::string& opaqueData) {
    ProtocolEvent event;
    event.type = ProtocolEventType::PING_ACK_RECEIVED;
    event.opaqueData = opaqueData;
    return event;
  }

  static ProtocolEvent goAwaySent(StreamId lastStreamId,
                                  ErrorCode errorCode,
                                  const std::string& debugData) {
    ProtocolEvent event;
    event.type = ProtocolEventType::GOAWAY_SENT;
    event.lastStreamId = lastStreamId;
    event.errorCode = errorCode;
    event.debugData = debugData;
    return event;
  }

  static ProtocolEvent responseCompleted(StreamId streamId, Response&& response) {
    ProtocolEvent event;
    event.type = ProtocolEventType::RESPONSE_COMPLETED;
    event.streamId = streamId;
    event.response = std::move(response);
    return event;
  }

  static ProtocolEvent responseFailed(StreamId streamId, ErrorCode errorCode) {
    ProtocolEvent event;
    event.type = ProtocolEventType::RESPONSE_FAILED;
    event.streamId = streamId;
    event.errorCode = errorCode;
    return event;
  }

  static ProtocolEvent connectionError(ErrorCode errorCode) {
    ProtocolEvent event;
    event.type = ProtocolEventType::CONNECTION_ERROR;
    event.errorCode = errorCode;
    return event;
  }

  static ProtocolEvent connectionClosed(ErrorCode errorCode) {
    ProtocolEvent event;
    event.type = ProtocolEventType::CONNECTION_CLOSED;
    event.errorCode = errorCode;
    return event;
  }

  static ProtocolEvent transportClosed() {
    ProtocolEvent event;
    event.type = ProtocolEventType::TRANSPORT_CLOSED;
    return event;
  }

  static ProtocolEvent goAwayReceived(StreamId lastStreamId,
                                      ErrorCode errorCode,
                                      const std::string& debugData) {
    ProtocolEvent event;
    event.type = ProtocolEventType::GOAWAY_RECEIVED;
    event.lastStreamId = lastStreamId;
    event.errorCode = errorCode;
    event.debugData = debugData;
    return event;
  }

  static ProtocolEvent streamOpened(StreamId streamId) {
    ProtocolEvent event;
    event.type = ProtocolEventType::STREAM_OPENED;
    event.streamId = streamId;
    return event;
  }

  static ProtocolEvent streamHeadersReceived(StreamId streamId, const Response& response) {
    ProtocolEvent event;
    event.type = ProtocolEventType::STREAM_HEADERS_RECEIVED;
    event.streamId = streamId;
    event.response = response;
    return event;
  }

  static ProtocolEvent streamResetReceived(StreamId streamId, ErrorCode errorCode) {
    ProtocolEvent event;
    event.type = ProtocolEventType::STREAM_RESET_RECEIVED;
    event.streamId = streamId;
    event.errorCode = errorCode;
    return event;
  }

  static ProtocolEvent streamClosed(StreamId streamId, ErrorCode errorCode) {
    ProtocolEvent event;
    event.type = ProtocolEventType::STREAM_CLOSED;
    event.streamId = streamId;
    event.errorCode = errorCode;
    return event;
  }

  static ProtocolEvent windowUpdateReceived(StreamId streamId, std::uint32_t increment) {
    ProtocolEvent event;
    event.type = ProtocolEventType::WINDOW_UPDATE_RECEIVED;
    event.streamId = streamId;
    event.windowSizeIncrement = increment;
    return event;
  }

  static ProtocolEvent pushPromiseReceived(StreamId streamId, StreamId promisedStreamId) {
    ProtocolEvent event;
    event.type = ProtocolEventType::PUSH_PROMISE_RECEIVED;
    event.streamId = streamId;
    event.promisedStreamId = promisedStreamId;
    return event;
  }

  static ProtocolEvent unknownFrameReceived(FrameType frameType,
                                            StreamId streamId,
                                            Flags flags,
                                            const std::string& opaqueData) {
    ProtocolEvent event;
    event.type = ProtocolEventType::UNKNOWN_FRAME_RECEIVED;
    event.frameType = frameType;
    event.streamId = streamId;
    event.flags = flags;
    event.opaqueData = opaqueData;
    return event;
  }
};

}  // namespace chttp2
