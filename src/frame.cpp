#include "chttp2/frame.hpp"

#include <cassert>
#include <cstdint>

namespace chttp2 {
FrameHeader FrameHeader::decode(ByteSpan data) {
  // frame header is 9 bytes
  assert(data.size() == 9);
  FrameHeader header;
  header.payloadLength = (static_cast<uint32_t>(data[0]) << 16) |
                         (static_cast<uint32_t>(data[1]) << 8) | static_cast<uint32_t>(data[2]);
  header.type = static_cast<FrameType>(data[3]);
  header.flags = data[4];
  header.streamId = (static_cast<uint32_t>(data[5]) << 24) |
                    (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 8) |
                    static_cast<uint32_t>(data[8]);

  // significant bit of streamId is reserved
  header.streamId &= 0x7FFFFFFF;
  return header;
}

ByteBuffer FrameHeader::encode(const FrameHeader& header) {
  ByteBuffer buf(9u);
  buf.append(static_cast<uint8_t>(header.payloadLength >> 16));
  buf.append(static_cast<uint8_t>(header.payloadLength >> 8));
  buf.append(static_cast<uint8_t>(header.payloadLength));
  buf.append(static_cast<uint8_t>(header.type));
  buf.append(header.flags);
  buf.append(static_cast<uint8_t>(header.streamId >> 24));
  buf.append(static_cast<uint8_t>(header.streamId >> 16));
  buf.append(static_cast<uint8_t>(header.streamId >> 8));
  buf.append(static_cast<uint8_t>(header.streamId));
  return buf;
}

DataFrame::DataFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    padLength = payload[0];
    payload.advance(1);
  }

  if (padLength > 0) {
    std::uint32_t dataSize = payload.size() - padLength;
    data = ByteBuffer::from(payload.begin(), payload.begin() + dataSize);
    payload.advance(dataSize);
    padding = ByteBuffer::from(payload.begin(), payload.end());
  } else {
    data = ByteBuffer::from(payload.begin(), payload.end());
  }
}

ByteBuffer DataFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    buf.append(padLength);
  }
  buf.append(data.begin(), data.end());
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    buf.append(padding.begin(), padding.end());
  }
  return buf;
}

HeadersFrame::HeadersFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    padLength = payload[0];
    payload.advance(1);
  }

  if (header.hasAnyFlag(FrameFlag::PRIORITY)) {
    streamDependency = payload.readAs<uint32_t>();
    weight = payload.readAs<uint8_t>();
  }

  if (padLength > 0) {
    std::uint32_t dataSize = payload.size() - padLength;
    data = ByteBuffer::from(payload.begin(), payload.begin() + dataSize);
    payload.advance(dataSize);
    padding = ByteBuffer::from(payload.begin(), payload.end());
  } else {
    data = ByteBuffer::from(payload.begin(), payload.end());
  }
}

ByteBuffer HeadersFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    buf.append(padLength);
  }
  if (header.hasAnyFlag(FrameFlag::PRIORITY)) {
    buf.append(streamDependency);
    buf.append(weight);
  }
  buf.append(data.begin(), data.end());
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    buf.append(padding.begin(), padding.end());
  }
  return buf;
}

RstStreamFrame::RstStreamFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  errorCode = payload.readAs<uint32_t>();
}

ByteBuffer RstStreamFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  buf.append(errorCode);
  return buf;
}

SettingFrame::SettingFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  if (payload.size() % 6 != 0) {
    return;
  }
  while (!payload.empty()) {
    auto id = payload.readAs<uint16_t>();
    auto value = payload.readAs<uint32_t>();
    settings.emplace_back(id, value);
  }
}

ByteBuffer SettingFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  for (const auto& setting : settings) {
    buf.append<uint16_t>(setting.first);
    buf.append<uint32_t>(setting.second);
  }
  return buf;
}

PushPromiseFrame::PushPromiseFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    padLength = payload[0];
    payload.advance(1);
  }

  promisedStreamId = payload.readAs<uint32_t>() & 0x7fffffffu;

  if (padLength > 0) {
    std::uint32_t dataSize = payload.size() - padLength;
    data = ByteBuffer::from(payload.begin(), payload.begin() + dataSize);
    payload.advance(dataSize);
    padding = ByteBuffer::from(payload.begin(), payload.end());
  } else {
    data = ByteBuffer::from(payload.begin(), payload.end());
  }
}

ByteBuffer PushPromiseFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    buf.append(padLength);
  }
  buf.append(promisedStreamId);
  buf.append(data.begin(), data.end());
  if (header.hasAnyFlag(FrameFlag::PADDED)) {
    buf.append(padding.begin(), padding.end());
  }
  return buf;
}

PingFrame::PingFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  data = ByteBuffer::from(payload.begin(), payload.end());
}

ByteBuffer PingFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  buf.append(data.begin(), data.end());
  return buf;
}

GoAwayFrame::GoAwayFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  lastStreamId = payload.readAs<uint32_t>() & 0x7fffffffu;
  errorCode = payload.readAs<uint32_t>();
  data = ByteBuffer::from(payload.begin(), payload.end());
}

ByteBuffer GoAwayFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  buf.append(lastStreamId);
  buf.append(errorCode);
  buf.append(data.begin(), data.end());
  return buf;
}

WindowUpdateFrame::WindowUpdateFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  windowSizeIncrement = payload.readAs<uint32_t>();
}

ByteBuffer WindowUpdateFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  buf.append(windowSizeIncrement);
  return buf;
}

ContinuationFrame::ContinuationFrame(const RawFrame& frame) {
  header = frame.header;
  ByteSpan payload = frame.data;
  data = ByteBuffer::from(payload.begin(), payload.end());
}

ByteBuffer ContinuationFrame::toBytes() const {
  ByteBuffer buf;
  auto headerBuf = FrameHeader::encode(header);
  buf.append(headerBuf.begin(), headerBuf.end());
  buf.append(data.begin(), data.end());
  return buf;
}

}  // namespace chttp2
