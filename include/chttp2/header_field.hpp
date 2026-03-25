#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

namespace chttp2 {

struct HeaderField {
  HeaderField() = default;
  HeaderField(std::string headerName, std::string headerValue)
      : name(std::move(headerName)), value(std::move(headerValue)) {}
  HeaderField(std::string headerName, std::string headerValue, bool sensitive)
      : name(std::move(headerName)), value(std::move(headerValue)), sensitivity(sensitive) {}

  std::string name;
  std::string value;

  // Sensitivity means that this header field should never be indexed
  bool sensitivity{false};

  // RFC 7541 §4.1: entry size = name length + value length + 32
  uint64_t size() const { return name.size() + value.size() + 32; }

  bool pseudo() const { return !name.empty() && name[0] == ':'; }

  std::string str() const { return name + ": " + value + (sensitivity ? " (sensitive)" : ""); }

  friend bool operator==(const HeaderField& a, const HeaderField& b) {
    return a.name == b.name && a.value == b.value;
  }

  friend bool operator!=(const HeaderField& a, const HeaderField& b) { return !(a == b); }

  friend bool operator<(const HeaderField& a, const HeaderField& b) {
    return std::tie(a.name, a.value) < std::tie(b.name, b.value);
  }
};

}  // namespace chttp2
