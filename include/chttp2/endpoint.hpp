#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace chttp2 {

enum class SocketType : std::uint8_t {
  TCP,
  UNIX,
};

struct Endpoint {
  // Connection target.
  std::string host;
  uint16_t port{0};
  bool useSSL{false};

  // Transport options.
  std::string certFile;
  std::string ip;        // Direct IP — skips DNS resolution.
  std::string sockFile;  // Unix socket path.
  SocketType type{SocketType::TCP};

  // The effective connection target: sockFile for UNIX, host or ip for TCP.
  std::string target() const {
    if (type == SocketType::UNIX) {
      return sockFile;
    }
    return host.empty() ? ip : host;
  }

  bool operator==(const Endpoint& other) const {
    if (type != other.type) {
      return false;
    }
    if (type == SocketType::UNIX) {
      return sockFile == other.sockFile;
    }
    return target() == other.target() && port == other.port && useSSL == other.useSSL;
  }

  bool operator!=(const Endpoint& other) const { return !(*this == other); }
};

struct EndpointHash {
  std::size_t operator()(const Endpoint& ep) const {
    std::size_t h = std::hash<std::string>()(ep.target());
    if (ep.type == SocketType::UNIX) {
      return h;
    }
    h ^= std::hash<uint16_t>()(ep.port) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<bool>()(ep.useSSL) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

class Connection;

// Helper for transferring a unique_ptr<Connection> across threads via lambda
// capture in C++11 (which doesn't support move-capture).
struct ConnectionHolder {
  std::unique_ptr<Connection> conn;
};

}  // namespace chttp2
