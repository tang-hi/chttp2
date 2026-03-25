#include "chttp2/frame.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "chttp2/http2_constants.hpp"
#include "chttp2/byte_buffer.hpp"
#include "chttp2/byte_span.hpp"

namespace {

chttp2::ByteBuffer makeBuffer(std::initializer_list<uint8_t> bytes) {
  return chttp2::ByteBuffer::from(bytes.begin(), bytes.end());
}

std::vector<uint8_t> toBytes(const chttp2::ByteBuffer& buf) {
  return std::vector<uint8_t>(buf.begin(), buf.end());
}

chttp2::RawFrame decodeEncodedFrame(const chttp2::ByteBuffer& encoded) {
  const size_t headerSize = chttp2::FrameHeader::encodedSize();
  if (encoded.size() < headerSize) {
    throw std::runtime_error("Encoded frame too short");
  }

  chttp2::ByteSpan headerSpan(encoded.begin(), static_cast<std::uint32_t>(headerSize));
  chttp2::FrameHeader header = chttp2::FrameHeader::decode(headerSpan);

  chttp2::ByteBuffer payload = chttp2::ByteBuffer::from(encoded.begin() + headerSize, encoded.end());
  return chttp2::RawFrame(header, std::move(payload));
}

}  // namespace

TEST_CASE("FrameHeader encode/decode roundtrip", "[frame]") {
  chttp2::FrameHeader header;
  header.payloadLength = 0x010203;
  header.type = chttp2::FrameType::HEADERS;
  header.flags = 0xA5;
  header.streamId = 0xFFFFFFFFU;  // high bit should be masked after decode

  chttp2::ByteBuffer encoded = chttp2::FrameHeader::encode(header);
  REQUIRE(encoded.size() == chttp2::FrameHeader::encodedSize());

  chttp2::FrameHeader decoded = chttp2::FrameHeader::decode(chttp2::ByteSpan(encoded));
  REQUIRE(decoded.payloadLength == header.payloadLength);
  REQUIRE(decoded.type == header.type);
  REQUIRE(decoded.flags == header.flags);
  REQUIRE(decoded.streamId == 0x7FFFFFFFU);
}

TEST_CASE("DataFrame roundtrip", "[frame]") {
  chttp2::DataFrame frame(3);
  frame.header.setEndStreamFlag();
  frame.data = makeBuffer({0x01, 0x02, 0x03, 0x04});
  frame.header.payloadLength = frame.data.size();

  chttp2::ByteBuffer encoded = frame.toBytes();
  REQUIRE(encoded.size() == chttp2::FrameHeader::encodedSize() + 4);

  chttp2::RawFrame raw = decodeEncodedFrame(encoded);
  chttp2::DataFrame decoded(raw);

  REQUIRE(decoded.header.type == chttp2::FrameType::DATA);
  REQUIRE(decoded.header.streamId == 3);
  REQUIRE(decoded.header.hasEndStreamFlag());
  REQUIRE(toBytes(decoded.data) == std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04}));
}

TEST_CASE("HeadersFrame roundtrip with priority", "[frame]") {
  chttp2::HeadersFrame frame(5);
  frame.header.setPriorityFlag();
  frame.header.setEndHeadersFlag();
  frame.streamDependency = 7;
  frame.weight = 9;
  frame.data = makeBuffer({0xAA, 0xBB});
  frame.header.payloadLength = 7;  // 4(dep)+1(weight)+2(data)

  chttp2::ByteBuffer encoded = frame.toBytes();
  chttp2::RawFrame raw = decodeEncodedFrame(encoded);
  chttp2::HeadersFrame decoded(raw);

  REQUIRE(decoded.header.type == chttp2::FrameType::HEADERS);
  REQUIRE(decoded.header.streamId == 5);
  REQUIRE(decoded.header.hasEndHeadersFlag());
  REQUIRE(decoded.streamDependency == 7);
  REQUIRE(decoded.weight == 9);
  REQUIRE(toBytes(decoded.data) == std::vector<uint8_t>({0xAA, 0xBB}));
}

TEST_CASE("SettingFrame roundtrip", "[frame]") {
  chttp2::SettingFrame frame;
  frame.header.streamId = 0;
  frame.settings.emplace_back(
      static_cast<uint16_t>(chttp2::Http2SettingParameter::HEADER_TABLE_SIZE), 4096U);
  frame.settings.emplace_back(static_cast<uint16_t>(chttp2::Http2SettingParameter::MAX_FRAME_SIZE),
                              16384U);
  frame.header.payloadLength = static_cast<uint32_t>(frame.settings.size()) * 6;

  chttp2::ByteBuffer encoded = frame.toBytes();
  chttp2::RawFrame raw = decodeEncodedFrame(encoded);
  chttp2::SettingFrame decoded(raw);

  REQUIRE(decoded.header.type == chttp2::FrameType::SETTINGS);
  REQUIRE(decoded.settings.size() == 2);
  REQUIRE(decoded.settings[0].first ==
          static_cast<uint16_t>(chttp2::Http2SettingParameter::HEADER_TABLE_SIZE));
  REQUIRE(decoded.settings[0].second == 4096U);
  REQUIRE(decoded.settings[1].first ==
          static_cast<uint16_t>(chttp2::Http2SettingParameter::MAX_FRAME_SIZE));
  REQUIRE(decoded.settings[1].second == 16384U);
}

TEST_CASE("Control frame roundtrip", "[frame]") {
  SECTION("RST_STREAM") {
    chttp2::RstStreamFrame frame(11);
    frame.errorCode = static_cast<uint32_t>(chttp2::ErrorCode::CANCEL);
    frame.header.payloadLength = 4;

    chttp2::RawFrame raw = decodeEncodedFrame(frame.toBytes());
    chttp2::RstStreamFrame decoded(raw);

    REQUIRE(decoded.header.type == chttp2::FrameType::RST_STREAM);
    REQUIRE(decoded.header.streamId == 11);
    REQUIRE(decoded.errorCode == static_cast<uint32_t>(chttp2::ErrorCode::CANCEL));
  }

  SECTION("PING") {
    chttp2::PingFrame frame;
    frame.data = makeBuffer({1, 2, 3, 4, 5, 6, 7, 8});
    frame.header.payloadLength = frame.data.size();

    chttp2::RawFrame raw = decodeEncodedFrame(frame.toBytes());
    chttp2::PingFrame decoded(raw);

    REQUIRE(decoded.header.type == chttp2::FrameType::PING);
    REQUIRE(toBytes(decoded.data) == std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8}));
  }

  SECTION("GOAWAY") {
    chttp2::GoAwayFrame frame;
    frame.lastStreamId = 15;
    frame.errorCode = static_cast<uint32_t>(chttp2::ErrorCode::NO_ERROR);
    frame.data = chttp2::ByteBuffer::from(std::string("debug"));
    frame.header.payloadLength = 8 + frame.data.size();

    chttp2::RawFrame raw = decodeEncodedFrame(frame.toBytes());
    chttp2::GoAwayFrame decoded(raw);

    REQUIRE(decoded.header.type == chttp2::FrameType::GOAWAY);
    REQUIRE(decoded.lastStreamId == 15);
    REQUIRE(decoded.errorCode == static_cast<uint32_t>(chttp2::ErrorCode::NO_ERROR));
    REQUIRE(std::string(decoded.data.begin(), decoded.data.end()) == "debug");
  }

  SECTION("WINDOW_UPDATE") {
    chttp2::WindowUpdateFrame frame(9);
    frame.windowSizeIncrement = 1024;
    frame.header.payloadLength = 4;

    chttp2::RawFrame raw = decodeEncodedFrame(frame.toBytes());
    chttp2::WindowUpdateFrame decoded(raw);

    REQUIRE(decoded.header.type == chttp2::FrameType::WINDOW_UPDATE);
    REQUIRE(decoded.header.streamId == 9);
    REQUIRE(decoded.windowSizeIncrement == 1024);
  }

  SECTION("CONTINUATION") {
    chttp2::ContinuationFrame frame(13);
    frame.header.setEndHeadersFlag();
    frame.data = makeBuffer({0x10, 0x20, 0x30});
    frame.header.payloadLength = frame.data.size();

    chttp2::RawFrame raw = decodeEncodedFrame(frame.toBytes());
    chttp2::ContinuationFrame decoded(raw);

    REQUIRE(decoded.header.type == chttp2::FrameType::CONTINUATION);
    REQUIRE(decoded.header.streamId == 13);
    REQUIRE(decoded.header.hasEndHeadersFlag());
    REQUIRE(toBytes(decoded.data) == std::vector<uint8_t>({0x10, 0x20, 0x30}));
  }
}
