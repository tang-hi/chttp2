#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "chttp2/byte_buffer.hpp"
#include "chttp2/byte_span.hpp"

namespace chttp2 {

struct Node {
  ~Node() {
    for (auto* child : children) {
      delete child;
    }
  }

  Node* children[256]{};
  bool leaf{false};
  uint8_t bits{0};  // how many bits this leaf consumed from the 8-bit chunk
  uint8_t symbol{0};
};

enum class HuffmanResult : uint8_t {
  OK,
  INVALID_CODE,
};

class Huffman {
 public:
  ByteBuffer encode(const std::string& str);
  HuffmanResult decode(ByteSpan data, std::string& result);
  uint64_t encodedLength(const std::string& str);

  Node* getRoot() {
    std::call_once(initFlag, buildTree);
    return root;
  }

  static void freeRoot() {
    delete root;
    root = nullptr;
  }

 private:
  static Node* root;
  static std::once_flag initFlag;
  static void buildTree();
  static std::vector<uint32_t> codes;
  static std::vector<uint8_t> lengths;
};

}  // namespace chttp2
