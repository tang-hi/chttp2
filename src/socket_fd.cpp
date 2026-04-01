#include "chttp2/socket_fd.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace chttp2 {

void SocketFd::reset(socket_t fd) {
  if (rawFd != INVALID_SOCKET) {
    ::close(rawFd);
  }
  rawFd = fd;
}

bool SocketFd::setNonBlocking(bool enable) {
  if (!valid()) {
    return false;
  }

  int flags = fcntl(rawFd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }

  if (enable) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }

  return fcntl(rawFd, F_SETFL, flags) == 0;
}

bool SocketFd::setKeepAlive(bool enable) {
  if (!valid()) {
    return false;
  }

  int value = enable ? 1 : 0;
  return setsockopt(rawFd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value)) == 0;
}

bool SocketFd::setKeepAliveParams(int idleSec, int intervalSec, int count) {
  if (!valid()) {
    return false;
  }
#if defined(__linux__)
  if (setsockopt(rawFd, IPPROTO_TCP, TCP_KEEPIDLE, &idleSec, sizeof(idleSec)) != 0) {
    return false;
  }
  if (setsockopt(rawFd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSec, sizeof(intervalSec)) != 0) {
    return false;
  }
  if (setsockopt(rawFd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
    return false;
  }
#elif defined(__APPLE__)
  if (setsockopt(rawFd, IPPROTO_TCP, TCP_KEEPALIVE, &idleSec, sizeof(idleSec)) != 0) {
    return false;
  }
  if (setsockopt(rawFd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSec, sizeof(intervalSec)) != 0) {
    return false;
  }
  if (setsockopt(rawFd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
    return false;
  }
#endif
  return true;
}

bool SocketFd::shutdownBoth() {
  if (!valid()) {
    return false;
  }
  return shutdown(rawFd, SHUT_RDWR) == 0;
}

ssize_t SocketFd::read(void* data, size_t len) const {
  if (!valid()) {
    return -1;
  }
  return recv(rawFd, data, len, 0);
}

ssize_t SocketFd::write(const void* data, size_t len) const {
  if (!valid()) {
    return -1;
  }
#if defined(__linux__)
  return send(rawFd, data, len, MSG_NOSIGNAL);
#else
  return send(rawFd, data, len, 0);
#endif
}

}  // namespace chttp2
