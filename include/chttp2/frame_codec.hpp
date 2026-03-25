#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chttp2/byte_span.hpp"
#include "chttp2/frame.hpp"

namespace chttp2 {

class FrameCodec {
 public:
  void feed(const std::uint8_t* data, std::size_t len);
  void feed(ByteSpan span);

  bool tryPop(RawFrame* frame);
  void clear();
  std::size_t bufferedBytes() const { return buffer.size(); }

 private:
  std::vector<std::uint8_t> buffer;
};

}  // namespace chttp2
