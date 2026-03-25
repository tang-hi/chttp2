#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace chttp2 {

class ByteSpan {
 public:
  ByteSpan() : ptr(nullptr), len(0), cur(0) {}
  ByteSpan(const std::uint8_t* data, std::uint32_t size) : ptr(data), len(size), cur(0) {}

  std::uint32_t size() const { return len - cur; }
  bool empty() const { return cur >= len; }
  std::uint32_t offset() const { return cur; }
  const std::uint8_t* data() const { return ptr + cur; }

  const std::uint8_t& operator[](std::uint32_t index) const {
    if (index >= len) {
      throw std::runtime_error("ByteSpan::operator[]: out of range");
    }
    return ptr[index];
  }

  void advance(std::uint32_t n) {
    if (cur + n > len) {
      throw std::runtime_error("ByteSpan::advance: out of range");
    }
    cur += n;
  }

  template <typename T>
  T peek() const {
    if (cur + sizeof(T) > len) {
      throw std::runtime_error("ByteSpan::peek: out of range");
    }
    T value = 0;
    auto nbytes = sizeof(T);
    for (std::size_t i = 0; i < nbytes; i++) {
      value = static_cast<T>((value << 8)) | static_cast<T>(ptr[cur + i]);
    }
    return value;
  }

  template <typename T>
  T readAs() {
    T value = peek<T>();
    advance(static_cast<std::uint32_t>(sizeof(T)));
    return value;
  }

  ByteSpan subspan(std::uint32_t off, std::uint32_t count) const {
    if (cur + off + count > len) {
      throw std::runtime_error("ByteSpan::subspan: out of range");
    }
    return ByteSpan(ptr + cur + off, count);
  }

  ByteSpan subspan(std::uint32_t off) const {
    if (cur + off > len) {
      throw std::runtime_error("ByteSpan::subspan: out of range");
    }
    return ByteSpan(ptr + cur + off, len - cur - off);
  }

  const std::uint8_t* begin() const { return ptr + cur; }
  const std::uint8_t* end() const { return ptr + len; }

  friend bool operator==(const ByteSpan& lhs, const ByteSpan& rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    return std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }

  friend bool operator!=(const ByteSpan& lhs, const ByteSpan& rhs) { return !(lhs == rhs); }

 private:
  const std::uint8_t* ptr;
  std::uint32_t len;
  std::uint32_t cur;
};

}  // namespace chttp2
