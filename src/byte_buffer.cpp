#include "chttp2/byte_buffer.hpp"

#include <algorithm>

namespace chttp2 {

ByteBuffer::ByteBuffer(std::uint32_t capacity)
    : ptr(capacity > 0 ? new std::uint8_t[capacity] : nullptr), len(0), cap(capacity) {}

ByteBuffer::~ByteBuffer() {
  delete[] ptr;
}

ByteBuffer::ByteBuffer(ByteBuffer&& other) noexcept
    : ptr(other.ptr), len(other.len), cap(other.cap) {
  other.ptr = nullptr;
  other.len = 0;
  other.cap = 0;
}

ByteBuffer& ByteBuffer::operator=(ByteBuffer&& other) noexcept {
  if (this != &other) {
    delete[] ptr;
    ptr = other.ptr;
    len = other.len;
    cap = other.cap;
    other.ptr = nullptr;
    other.len = 0;
    other.cap = 0;
  }
  return *this;
}

void ByteBuffer::ensureCapacity(std::uint32_t needed) {
  if (needed <= cap) {
    return;
  }
  auto newCap = std::max(8u, cap * 2);
  while (newCap < needed) {
    newCap *= 2;
  }
  auto* newPtr = new std::uint8_t[newCap];
  if (ptr != nullptr) {
    std::copy(ptr, ptr + len, newPtr);
    delete[] ptr;
  }
  ptr = newPtr;
  cap = newCap;
}

}  // namespace chttp2
