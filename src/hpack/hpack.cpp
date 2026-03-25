#include "chttp2/hpack/hpack.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

#include "chttp2/log.hpp"

namespace chttp2 {
Huffman Hpack::huffman;
//======== Implement of binary format of HPACK =========
void Hpack::encodeVInt(ByteBuffer& buffer, std::uint8_t prefix, std::uint64_t value) {
  auto maxPrefix = static_cast<std::uint8_t>((1 << prefix) - 1);
  if (value < maxPrefix) {
    buffer.append(static_cast<uint8_t>(value));
    return;
  }
  buffer.append(maxPrefix);
  value -= maxPrefix;
  while (value >= 128) {
    buffer.append<uint8_t>(0x80 | (value & 0x7f));
    value >>= 7;
  }
  buffer.append(static_cast<std::uint8_t>(value));
}

void Hpack::encodeString(ByteBuffer& buffer, const std::string& str) {
  auto huffmanLength = huffman.encodedLength(str);
  if (huffmanLength < str.size()) {
    auto flagPos = buffer.size();
    encodeVInt(buffer, 7, huffmanLength);
    auto huffmanStr = huffman.encode(str);
    buffer.append(huffmanStr.begin(), huffmanStr.end());
    buffer[flagPos] |= 0x80;
    return;
  }
  encodeVInt(buffer, 7, str.size());
  buffer.append(str.begin(), str.end());
}

void Hpack::encodeIndexedHeader(ByteBuffer& buffer, std::uint64_t index) {
  auto flagPos = buffer.size();
  encodeVInt(buffer, 7, index);
  buffer[flagPos] |= 0x80;
}

void Hpack::encodeIndexLiteralHeader(ByteBuffer& buffer,
                                     std::uint64_t index,
                                     const HeaderField& header) {
  assert(header.sensitivity == false);
  auto flagPos = buffer.size();
  // header's name is already indexed
  if (index > 0) {
    encodeVInt(buffer, 6, index);
    buffer[flagPos] |= 0x40;
  } else {
    assert(index == 0);
    buffer.append<uint8_t>(0x40);
    encodeString(buffer, header.name);
  }
  encodeString(buffer, header.value);
}

void Hpack::encodeUnIndexLiteralHeader(ByteBuffer& buffer,
                                       std::uint64_t index,
                                       const HeaderField& header) {
  assert(header.sensitivity == false);

  // header's name is already indexed
  if (index > 0) {
    encodeVInt(buffer, 4, index);
  } else {
    assert(index == 0);
    buffer.append<uint8_t>(0);
    encodeString(buffer, header.name);
  }

  encodeString(buffer, header.value);
}

void Hpack::encodeNeverIndexLiteralHeader(ByteBuffer& buffer,
                                          std::uint64_t index,
                                          const HeaderField& header) {
  assert(header.sensitivity == true);

  auto flagPos = buffer.size();
  // header's name is already indexed
  if (index > 0) {
    encodeVInt(buffer, 4, index);
    buffer[flagPos] |= 0x10;
  } else {
    buffer.append<uint8_t>(0x10);
    encodeString(buffer, header.name);
  }

  encodeString(buffer, header.value);
}

void Hpack::encodeTableSizeUpdate(ByteBuffer& buffer, std::uint64_t size) {
  auto flagPos = buffer.size();
  encodeVInt(buffer, 5, size);
  buffer[flagPos] |= 0x20;
}

bool Hpack::couldIndex(const HeaderField& header) {
  if (header.sensitivity) {
    return false;
  }
  return encodeTable.dynamicTableBytes() + header.size() <= encodeTable.getMaxSize();
}

ByteBuffer Hpack::encode(const std::vector<HeaderField>& headers) {
  CHTTP2_LOG_DEBUG("hpack: encode %zu headers, table_size=%zu/%zu",
                   headers.size(),
                   encodeTable.dynamicTableBytes(),
                   encodeTable.getMaxSize());
  ByteBuffer buffer(128u);
  if (encodeTableDirty) {
    encodeTableDirty = false;
    if (encodeMinSize < encodeTable.getMaxSize()) {
      CHTTP2_LOG_DEBUG("hpack: encode table size update min=%zu max=%zu",
                       encodeMinSize,
                       encodeTable.getMaxSize());
      encodeTableSizeUpdate(buffer, encodeMinSize);
    }
    encodeMinSize = std::numeric_limits<std::size_t>::max();
    encodeTableSizeUpdate(buffer, encodeTable.getMaxSize());
  }

  for (const auto& header : headers) {
    if (header.sensitivity) {
      std::uint64_t idx = 0;
      encodeTable.find(header, idx);
      CHTTP2_LOG_DEBUG("hpack: encode never-index '%s' idx=%llu",
                       header.name.c_str(),
                       static_cast<unsigned long long>(idx));
      encodeNeverIndexLiteralHeader(buffer, idx, header);
      continue;
    }

    std::uint64_t idx = 0;
    auto exactMatch = encodeTable.find(header, idx);
    if (exactMatch) {
      CHTTP2_LOG_DEBUG("hpack: encode indexed '%s' idx=%llu",
                       header.name.c_str(),
                       static_cast<unsigned long long>(idx));
      encodeIndexedHeader(buffer, idx);
    } else if (couldIndex(header)) {
      CHTTP2_LOG_DEBUG("hpack: encode incremental '%s: %s' idx=%llu",
                       header.name.c_str(),
                       header.value.c_str(),
                       static_cast<unsigned long long>(idx));
      encodeIndexLiteralHeader(buffer, idx, header);
      encodeTable.insert(header);
    } else {
      CHTTP2_LOG_DEBUG("hpack: encode literal '%s: %s' idx=%llu",
                       header.name.c_str(),
                       header.value.c_str(),
                       static_cast<unsigned long long>(idx));
      encodeUnIndexLiteralHeader(buffer, idx, header);
    }
  }
  return buffer;
}

bool Hpack::decode(ByteSpan data, std::vector<HeaderField>& headers) {
  CHTTP2_LOG_DEBUG("hpack: decode %zu bytes, table_size=%zu/%zu",
                   data.size(),
                   decodeTable.dynamicTableBytes(),
                   decodeTable.getMaxSize());
  // Reset per-header-block state. Each decode() call is a new header block,
  // so table size updates are only valid at the beginning.
  firstField = true;

  // An empty header block is valid in HPACK (encodes zero headers)
  if (data.empty()) {
    return true;
  }

  decodeBuffer = data;

  while (!decodeBuffer.empty()) {
    auto before = headers.size();
    if (!decodeHeader(headers)) {
      CHTTP2_LOG_ERROR(
          "hpack: decode failed at byte %zu/%zu", data.size() - decodeBuffer.size(), data.size());
      return false;
    }
    // Only clear firstField after an actual header field, not after a table size update.
    // RFC 7541 §4.2: multiple size updates may appear at the beginning.
    if (headers.size() > before) {
      firstField = false;
    }
  }

  CHTTP2_LOG_DEBUG("hpack: decoded %zu headers", headers.size());
  return true;
}

bool Hpack::decodeHeader(std::vector<HeaderField>& headers) {
  auto flag = decodeBuffer.peek<uint8_t>();
  if ((flag >> 7) == 1) {
    // 1xxxxxxx → indexed header field
    return decodeIndexedHeader(headers);
  } else if ((flag >> 6) == 1) {
    // 01xxxxxx → literal with incremental indexing
    return decodeHeaderField(headers, 6, true, false);
  } else if ((flag >> 5) == 1) {
    // 001xxxxx → dynamic table size update
    // RFC 7541 §6.3 / RFC 9113 §4.3: MUST only appear at the beginning.
    if (!firstField) {
      return false;
    }
    return decodeTableSizeUpdate();
  } else if ((flag >> 4) == 1) {
    // 0001xxxx → literal never indexed
    return decodeHeaderField(headers, 4, false, true);
  } else {
    // 0000xxxx → literal without indexing
    return decodeHeaderField(headers, 4, false, false);
  }
}

bool Hpack::decodeIndexedHeader(std::vector<HeaderField>& headers) {
  uint64_t index = 0;
  if (!decodeVInt(decodeBuffer, 7, index)) {
    CHTTP2_LOG_ERROR("hpack: indexed header — bad varint");
    return false;
  }

  HeaderField header;
  if (!decodeTable.at(index, header)) {
    CHTTP2_LOG_ERROR("hpack: indexed header — index %llu out of range",
                     static_cast<unsigned long long>(index));
    return false;
  }

  CHTTP2_LOG_DEBUG("hpack: decode indexed [%llu] '%s: %s'",
                   static_cast<unsigned long long>(index),
                   header.name.c_str(),
                   header.value.c_str());
  headers.push_back(std::move(header));
  return true;
}

bool Hpack::decodeTableSizeUpdate() {
  uint64_t size = 0;
  if (!decodeVInt(decodeBuffer, 5, size)) {
    CHTTP2_LOG_ERROR("hpack: table size update — bad varint");
    return false;
  }
  // RFC 7541 Section 6.3: new max MUST NOT exceed the protocol-level limit
  if (size > decodeTableLimit) {
    CHTTP2_LOG_ERROR("hpack: table size update %llu exceeds limit %llu",
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(decodeTableLimit));
    return false;
  }
  CHTTP2_LOG_DEBUG("hpack: decode table size update -> %llu",
                   static_cast<unsigned long long>(size));
  decodeTable.setMaxSize(size);
  return true;
}

bool Hpack::decodeHeaderField(std::vector<HeaderField>& headers,
                              std::uint8_t prefix,
                              bool indexed,
                              bool sensitive) {
  uint64_t index = 0;

  if (!decodeVInt(decodeBuffer, prefix, index)) {
    CHTTP2_LOG_ERROR("hpack: literal header — bad name index varint");
    return false;
  }

  HeaderField header;
  // none was indexed
  if (index == 0) {
    if (!decodeString(decodeBuffer, header.name)) {
      CHTTP2_LOG_ERROR("hpack: literal header — bad name string");
      return false;
    }

    if (!decodeString(decodeBuffer, header.value)) {
      CHTTP2_LOG_ERROR("hpack: literal header — bad value string for '%s'", header.name.c_str());
      return false;
    }

  } else {  // name was indexed
    HeaderField indexedHeader;
    if (!decodeTable.at(index, indexedHeader)) {
      CHTTP2_LOG_ERROR("hpack: literal header — name index %llu out of range",
                       static_cast<unsigned long long>(index));
      return false;
    }
    header.name = std::move(indexedHeader.name);
    if (!decodeString(decodeBuffer, header.value)) {
      CHTTP2_LOG_ERROR("hpack: literal header — bad value string for '%s'", header.name.c_str());
      return false;
    }
  }
  header.sensitivity = sensitive;
  if (indexed) {
    CHTTP2_LOG_DEBUG(
        "hpack: decode incremental '%s: %s'", header.name.c_str(), header.value.c_str());
    decodeTable.insert(header);
  } else {
    CHTTP2_LOG_DEBUG("hpack: decode literal '%s: %s'%s",
                     header.name.c_str(),
                     header.value.c_str(),
                     sensitive ? " (sensitive)" : "");
  }
  headers.push_back(std::move(header));

  return true;
}

bool Hpack::decodeVInt(ByteSpan& buffer, std::uint8_t prefix, std::uint64_t& value) {
  if (prefix < 1 || prefix > 8) {
    return false;
  }
  if (buffer.empty()) {
    return false;
  }

  // Read from a copy so that on failure (truncated data, overflow)
  // the caller's buffer position is left untouched.
  ByteSpan view = buffer;
  auto bytesBefore = view.size();

  auto maxPrefix = static_cast<uint8_t>((1 << prefix) - 1);
  value = view.readAs<uint8_t>() & maxPrefix;
  if (value < maxPrefix) {
    buffer.advance(bytesBefore - view.size());
    return true;
  }

  // RFC 7541 Section 5.1: continuation bytes use shift starting from 0,
  // not from the prefix bit width. Each byte contributes 7 bits:
  //   I = prefix_value + (B0 & 127) * 2^0 + (B1 & 127) * 2^7 + ...
  std::uint8_t shift = 0;
  while (!view.empty()) {
    auto byte = view.readAs<uint8_t>();
    value += (byte & 0x7ful) << shift;
    if ((byte & 0x80) == 0) {
      buffer.advance(bytesBefore - view.size());
      return true;
    }
    shift += 7;
    if (shift >= 63) {
      return false;  // overflow — caller's buffer untouched
    }
  }

  // incomplete varint — buffer exhausted mid-sequence, caller's buffer untouched
  return false;
}

bool Hpack::decodeString(ByteSpan& buffer, std::string& str) {
  if (buffer.empty()) {
    return false;
  }

  bool isHuffman = buffer.peek<uint8_t>() & 0x80;
  std::uint64_t length = 0;
  if (!decodeVInt(buffer, 7, length)) {
    return false;
  }

  // needs more data
  if (buffer.size() < length) {
    return false;
  }

  if (isHuffman) {
    std::string huffmanStr;
    auto code = huffman.decode(buffer.subspan(0, static_cast<uint32_t>(length)), huffmanStr);
    if (code != HuffmanResult::OK) {
      CHTTP2_LOG_WARN("HPACK: invalid huffman code in header block");
      return false;
    }
    str.insert(str.end(), huffmanStr.begin(), huffmanStr.end());
  } else {
    str.insert(str.end(), buffer.begin(), buffer.begin() + length);
  }
  buffer.advance(static_cast<uint32_t>(length));
  return true;
}

}  // namespace chttp2
