#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chttp2/byte_buffer.hpp"
#include "chttp2/byte_span.hpp"
#include "chttp2/http2_constants.hpp"
namespace chttp2 {

struct FrameHeader {
  std::uint32_t payloadLength;
  FrameType type;
  Flags flags;
  StreamId streamId;

  FrameHeader() : payloadLength(0), type(FrameType::INVALID), flags(0), streamId(0) {}
  explicit FrameHeader(FrameType frameType)
      : payloadLength(0), type(frameType), flags(0), streamId(0) {}

  FrameHeader(FrameType frameType, StreamId id)
      : payloadLength(0), type(frameType), flags(0), streamId(id) {}

  static constexpr size_t encodedSize() { return 9; }

  void retainFlag(FrameFlag validFlag) { flags = (flags & static_cast<uint8_t>(validFlag)); }

  bool hasAnyFlag(FrameFlag validFlag) const {
    return (flags & static_cast<uint8_t>(validFlag)) != 0;
  }

  void setEndStreamFlag() { flags |= static_cast<uint8_t>(FrameFlag::END_STREAM); }

  void setAckFlag() { flags |= static_cast<uint8_t>(FrameFlag::ACK); }

  void setEndHeadersFlag() { flags |= static_cast<uint8_t>(FrameFlag::END_HEADERS); }

  bool hasEndHeadersFlag() const { return hasAnyFlag(FrameFlag::END_HEADERS); }

  bool hasEndStreamFlag() const { return hasAnyFlag(FrameFlag::END_STREAM); }

  bool hasAckFlag() const { return hasAnyFlag(FrameFlag::ACK); }

  void setPaddedFlag() { flags |= static_cast<uint8_t>(FrameFlag::PADDED); }

  void setPriorityFlag() { flags |= static_cast<uint8_t>(FrameFlag::PRIORITY); }

  FrameType getType() const { return type; }

  static FrameHeader decode(ByteSpan data);

  static ByteBuffer encode(const FrameHeader& header);
};

struct RawFrame {
  RawFrame() = default;
  RawFrame(FrameHeader frameHeader, ByteBuffer body) : header(frameHeader), data(std::move(body)) {}
  FrameHeader header;
  ByteBuffer data;
};

struct DataFrame {
  DataFrame() : header(FrameType::DATA) {}
  explicit DataFrame(StreamId streamId) : header(FrameType::DATA, streamId) {}
  explicit DataFrame(const RawFrame& frame);
  StreamId getStreamId() const { return header.streamId; }

  FrameHeader header;
  uint8_t padLength = 0;
  ByteBuffer data;
  ByteBuffer padding;
  ByteBuffer toBytes() const;
};

struct HeadersFrame {
  HeadersFrame() : header(FrameType::HEADERS) {}
  explicit HeadersFrame(StreamId streamId) : header(FrameType::HEADERS, streamId) {}
  explicit HeadersFrame(const RawFrame& frame);

  FrameHeader header;
  uint8_t padLength = 0;
  uint32_t streamDependency = 0;
  uint8_t weight = 0;
  ByteBuffer data;
  ByteBuffer padding;
  ByteBuffer toBytes() const;
};

// deprecated
struct PriorityFrame {
  PriorityFrame() = default;
  explicit PriorityFrame(StreamId /*unused*/) {}
  explicit PriorityFrame(const RawFrame& frame);
  ByteBuffer toBytes() const;
};

struct RstStreamFrame {
  RstStreamFrame() : header(FrameType::RST_STREAM) {}
  explicit RstStreamFrame(StreamId streamId) : header(FrameType::RST_STREAM, streamId) {}
  explicit RstStreamFrame(const RawFrame& frame);

  FrameHeader header;
  std::uint32_t errorCode{0};
  ByteBuffer toBytes() const;
};

struct SettingFrame {
  SettingFrame() : header(FrameType::SETTINGS) {}
  explicit SettingFrame(StreamId streamId) : header(FrameType::SETTINGS, streamId) {}
  explicit SettingFrame(const RawFrame& frame);

  FrameHeader header;
  std::vector<std::pair<std::uint16_t, std::uint32_t>> settings;
  ByteBuffer toBytes() const;
};

struct PushPromiseFrame {
  PushPromiseFrame() : header(FrameType::PUSH_PROMISE) {}
  explicit PushPromiseFrame(StreamId streamId) : header(FrameType::PUSH_PROMISE, streamId) {}
  explicit PushPromiseFrame(const RawFrame& frame);

  FrameHeader header;
  std::uint8_t padLength = 0;
  std::uint32_t promisedStreamId;
  ByteBuffer data;
  ByteBuffer padding;
  ByteBuffer toBytes() const;
};

struct PingFrame {
  PingFrame() : header(FrameType::PING) {}
  explicit PingFrame(StreamId streamId) : header(FrameType::PING, streamId) {}
  explicit PingFrame(const RawFrame& frame);

  FrameHeader header;
  ByteBuffer data;
  ByteBuffer toBytes() const;
};

struct GoAwayFrame {
  GoAwayFrame() : header(FrameType::GOAWAY) {}
  explicit GoAwayFrame(StreamId streamId) : header(FrameType::GOAWAY, streamId) {}
  explicit GoAwayFrame(const RawFrame& frame);
  FrameHeader header;
  std::uint32_t lastStreamId;
  std::uint32_t errorCode;
  ByteBuffer data;
  ByteBuffer toBytes() const;
};

struct WindowUpdateFrame {
  WindowUpdateFrame() : header(FrameType::WINDOW_UPDATE) {}
  explicit WindowUpdateFrame(StreamId streamId) : header(FrameType::WINDOW_UPDATE, streamId) {}
  explicit WindowUpdateFrame(const RawFrame& frame);
  FrameHeader header;
  std::uint32_t windowSizeIncrement;
  ByteBuffer toBytes() const;
};

struct ContinuationFrame {
  ContinuationFrame() : header(FrameType::CONTINUATION) {}
  ContinuationFrame(StreamId streamId) : header(FrameType::CONTINUATION, streamId) {}
  explicit ContinuationFrame(const RawFrame& frame);
  FrameHeader header;
  ByteBuffer data;
  ByteBuffer toBytes() const;
};

template <typename Frame>
std::string summarizeFrame(const Frame& frame) {
  std::string summary;
  summary += "Frame Type: ";
  summary += FrameTypeString(frame.header.type);
  summary += " Stream ID: ";
  summary += std::to_string(frame.header.streamId);
  summary += " Payload Length: ";
  summary += std::to_string(frame.header.payloadLength);
  return summary;
}

template <>
inline std::string summarizeFrame<RstStreamFrame>(const RstStreamFrame& frame) {
  std::string summary;
  summary += "Frame Type: ";
  summary += frameTypeString(frame.header.type);
  summary += " Stream ID: ";
  summary += std::to_string(frame.header.streamId);
  summary += " Error Code: ";
  summary += std::to_string(frame.errorCode);
  return summary;
}

template <>
inline std::string summarizeFrame<SettingFrame>(const SettingFrame& frame) {
  std::string summary;
  summary += "Frame Type: ";
  summary += frameTypeString(frame.header.type);
  summary += " Stream ID: ";
  summary += std::to_string(frame.header.streamId);
  summary += " Settings: ";
  for (const auto& setting : frame.settings) {
    summary += std::to_string(setting.first);
    summary += ":";
    summary += std::to_string(setting.second);
    summary += " ";
  }
  return summary;
}

template <>
inline std::string summarizeFrame<GoAwayFrame>(const GoAwayFrame& frame) {
  std::string summary;
  summary += "Frame Type: ";
  summary += frameTypeString(frame.header.type);
  summary += " Stream ID: ";
  summary += std::to_string(frame.header.streamId);
  summary += " Last Stream ID: ";
  summary += std::to_string(frame.lastStreamId);
  summary += " Error Code: ";
  summary += std::to_string(frame.errorCode);
  return summary;
}

template <>
inline std::string summarizeFrame<WindowUpdateFrame>(const WindowUpdateFrame& frame) {
  std::string summary;
  summary += "Frame Type: ";
  summary += frameTypeString(frame.header.type);
  summary += " Stream ID: ";
  summary += std::to_string(frame.header.streamId);
  summary += " Window Size Increment: ";
  summary += std::to_string(frame.windowSizeIncrement);
  return summary;
}

}  // namespace chttp2
