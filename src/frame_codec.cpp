#include "chttp2/frame_codec.hpp"

namespace chttp2 {

void FrameCodec::feed(const std::uint8_t* data, std::size_t len) {
  if (!data || len == 0) {
    return;
  }

  buffer.insert(buffer.end(), data, data + len);
}

void FrameCodec::feed(ByteSpan span) {
  if (span.empty()) {
    return;
  }
  feed(span.begin(), span.size());
}

bool FrameCodec::tryPop(RawFrame* frame) {
  if (!frame) {
    return false;
  }
  if (buffer.size() < FrameHeader::encodedSize()) {
    return false;
  }

  const std::size_t headerSize = FrameHeader::encodedSize();
  ByteSpan headerSpan(buffer.data(), static_cast<std::uint32_t>(headerSize));
  FrameHeader header = FrameHeader::decode(headerSpan);
  std::size_t totalSize = headerSize + static_cast<std::size_t>(header.payloadLength);
  if (buffer.size() < totalSize) {
    return false;
  }

  ByteBuffer payload = ByteBuffer::from(buffer.data() + headerSize, buffer.data() + totalSize);
  frame->header = header;
  frame->data = std::move(payload);

  buffer.erase(buffer.begin(),
               buffer.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(totalSize));
  return true;
}

void FrameCodec::clear() {
  buffer.clear();
}

}  // namespace chttp2
