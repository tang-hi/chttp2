#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>

#include "chttp2/byte_span.hpp"

namespace chttp2 {

class ByteBuffer {
 public:
  ByteBuffer() : ptr(nullptr), len(0), cap(0) {}
  explicit ByteBuffer(std::uint32_t capacity);
  ~ByteBuffer();

  ByteBuffer(const ByteBuffer&) = delete;
  ByteBuffer& operator=(const ByteBuffer&) = delete;
  ByteBuffer(ByteBuffer&& other) noexcept;
  ByteBuffer& operator=(ByteBuffer&& other) noexcept;

  std::uint32_t size() const { return len; }
  std::uint32_t capacity() const { return cap; }
  bool empty() const { return len == 0; }
  void clear() { len = 0; }

  std::uint8_t& operator[](std::uint32_t index) {
    if (index >= len) {
      throw std::runtime_error("ByteBuffer::operator[]: out of range");
    }
    return ptr[index];
  }

  const std::uint8_t& operator[](std::uint32_t index) const {
    if (index >= len) {
      throw std::runtime_error("ByteBuffer::operator[]: out of range");
    }
    return ptr[index];
  }

  template <typename T>
  void append(T value) {
    ensureCapacity(len + static_cast<std::uint32_t>(sizeof(T)));
    auto nbytes = static_cast<std::uint32_t>(sizeof(T));
    for (std::uint32_t i = 0; i < nbytes; i++) {
      ptr[len + i] = static_cast<std::uint8_t>(value >> ((nbytes - i - 1) * 8));
    }
    len += nbytes;
  }

  template <typename Iterator>
  void append(Iterator begin, Iterator end) {
    auto count = static_cast<std::uint32_t>(std::distance(begin, end));
    if (count == 0) {
      return;
    }
    ensureCapacity(len + count);
    std::copy(begin, end, ptr + len);
    len += count;
  }

  template <typename T>
  void writeAt(T value, std::uint32_t offset) {
    if (offset + sizeof(T) > len) {
      throw std::runtime_error("ByteBuffer::writeAt: out of range");
    }
    auto nbytes = static_cast<std::uint32_t>(sizeof(T));
    for (std::uint32_t i = 0; i < nbytes; i++) {
      ptr[offset + i] = static_cast<std::uint8_t>(value >> ((nbytes - i - 1) * 8));
    }
  }

  // Factory methods
  template <typename Iterator>
  static ByteBuffer from(Iterator begin, Iterator end) {
    ByteBuffer buf;
    buf.append(begin, end);
    return buf;
  }

  template <typename Container>
  static ByteBuffer from(const Container& c) {
    return from(c.begin(), c.end());
  }

  // Conversion to ByteSpan
  ByteSpan span() const { return ByteSpan(ptr, len); }
  operator ByteSpan() const { return span(); }  // NOLINT(google-explicit-constructor)

  // Iterators
  std::uint8_t* begin() { return ptr; }
  std::uint8_t* end() { return ptr + len; }
  const std::uint8_t* begin() const { return ptr; }
  const std::uint8_t* end() const { return ptr + len; }

  friend bool operator==(const ByteBuffer& lhs, const ByteBuffer& rhs) {
    if (lhs.len != rhs.len) {
      return false;
    }
    return std::equal(lhs.ptr, lhs.ptr + lhs.len, rhs.ptr);
  }

  friend bool operator!=(const ByteBuffer& lhs, const ByteBuffer& rhs) { return !(lhs == rhs); }

 private:
  void ensureCapacity(std::uint32_t needed);

  std::uint8_t* ptr;
  std::uint32_t len;
  std::uint32_t cap;
};

}  // namespace chttp2
