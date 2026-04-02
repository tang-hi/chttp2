#include "chttp2/socket_fd.hpp"

namespace chttp2 {

void SocketFd::reset(socket_t fd) {
  if (rawFd != INVALID_SOCKET) {
    closeSocket(rawFd);
  }
  rawFd = fd;
}

bool SocketFd::setNonBlocking(bool enable) {
  if (!valid()) {
    return false;
  }
  return chttp2::setNonBlocking(rawFd, enable);
}

bool SocketFd::setKeepAlive(bool enable) {
  if (!valid()) {
    return false;
  }
  return chttp2::setKeepAlive(rawFd, enable);
}

bool SocketFd::setKeepAliveParams(int idleSec, int intervalSec, int count) {
  if (!valid()) {
    return false;
  }
  return chttp2::setKeepAliveParams(rawFd, idleSec, intervalSec, count);
}

bool SocketFd::shutdownBoth() {
  if (!valid()) {
    return false;
  }
  return chttp2::shutdownBoth(rawFd);
}

ssize_t SocketFd::read(void* data, size_t len) const {
  if (!valid()) {
    return -1;
  }
  return socketRecv(rawFd, data, len);
}

ssize_t SocketFd::write(const void* data, size_t len) const {
  if (!valid()) {
    return -1;
  }
  return socketSend(rawFd, data, len);
}

}  // namespace chttp2
