#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "chttp2/byte_buffer.hpp"
#include "chttp2/byte_span.hpp"
#include "chttp2/header_field.hpp"
#include "chttp2/hpack/huffman.hpp"
#include "chttp2/hpack/table.hpp"

namespace chttp2 {

class Hpack {
 public:
  Hpack() = default;
  ~Hpack() = default;

  void reset() {
    decodeBuffer = ByteSpan{};
    firstField = true;
    decodeTable = HpackTable{};
    decodeTableLimit = INIT_MAX_TABLE_SIZE;
    encodeTable = HpackTable{};
    encodeTableDirty = false;
    encodeMinSize = std::numeric_limits<std::size_t>::max();
  }

  ByteBuffer encode(const std::vector<HeaderField>& headers);
  bool decode(ByteSpan data, std::vector<HeaderField>& headers);

  // We send SETTINGS{HEADER_TABLE_SIZE=limit} → constrains our own decoder
  void setDecodeTableLimit(std::size_t limit) {
    decodeTableLimit = limit;
    decodeTable.setMaxSize(limit);
  }

  // Peer sends SETTINGS{HEADER_TABLE_SIZE=size} → constrains our encoder
  void setEncodeTableSize(std::size_t size) {
    encodeMinSize = std::min(size, encodeMinSize);
    encodeTableDirty = true;
    encodeTable.setMaxSize(size);
  }

 private:
  static void encodeVInt(ByteBuffer& buffer, uint8_t prefix, uint64_t value);
  static bool decodeVInt(ByteSpan& buffer, uint8_t prefix, uint64_t& value);

  static void encodeString(ByteBuffer& buffer, const std::string& str);
  static bool decodeString(ByteSpan& buffer, std::string& str);

  static void encodeIndexedHeader(ByteBuffer& buffer, uint64_t index);

  static void encodeIndexLiteralHeader(ByteBuffer& buffer,
                                       uint64_t index,
                                       const HeaderField& header);

  static void encodeUnIndexLiteralHeader(ByteBuffer& buffer,
                                         uint64_t index,
                                         const HeaderField& header);

  static void encodeNeverIndexLiteralHeader(ByteBuffer& buffer,
                                            uint64_t index,
                                            const HeaderField& header);

  static void encodeTableSizeUpdate(ByteBuffer& buffer, uint64_t size);
  bool decodeHeader(std::vector<HeaderField>& headers);
  bool decodeIndexedHeader(std::vector<HeaderField>& headers);
  bool decodeHeaderField(std::vector<HeaderField>& headers,
                         uint8_t prefix,
                         bool indexed,
                         bool sensitive);
  bool decodeTableSizeUpdate();

  bool couldIndex(const HeaderField& header);

  // Decode state
  ByteSpan decodeBuffer;
  bool firstField{true};
  HpackTable decodeTable;
  uint64_t decodeTableLimit{INIT_MAX_TABLE_SIZE};

  // Encode state
  HpackTable encodeTable;
  bool encodeTableDirty{false};
  std::size_t encodeMinSize{std::numeric_limits<std::size_t>::max()};

  static Huffman huffman;
};

}  // namespace chttp2
