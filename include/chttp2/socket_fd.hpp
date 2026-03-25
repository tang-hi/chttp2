#pragma once

#include <cstddef>
#include <sys/types.h>

namespace chttp2 {

using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;

class SocketFd {
 public:
  SocketFd() = default;
  explicit SocketFd(socket_t fd) : rawFd(fd) {}
  ~SocketFd() { reset(); }

  SocketFd(const SocketFd&) = delete;
  SocketFd& operator=(const SocketFd&) = delete;

  SocketFd(SocketFd&& other) noexcept : rawFd(other.release()) {}
  SocketFd& operator=(SocketFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  bool valid() const { return rawFd != INVALID_SOCKET; }
  socket_t get() const { return rawFd; }

  socket_t release() {
    socket_t value = rawFd;
    rawFd = INVALID_SOCKET;
    return value;
  }

  void reset(socket_t fd = INVALID_SOCKET);

  bool setNonBlocking(bool enable);
  bool setKeepAlive(bool enable);
  bool setKeepAliveParams(int idleSec, int intervalSec, int count);
  bool shutdownBoth();

  ssize_t read(void* data, size_t len) const;
  ssize_t write(const void* data, size_t len) const;

 private:
  socket_t rawFd{INVALID_SOCKET};
};

}  // namespace chttp2
