#include "chttp2/platform.hpp"

#if defined(_WIN32)
#include <mstcpip.h>
#else
#include <poll.h>
#endif

namespace chttp2 {

// ============================================================================
// Socket options
// ============================================================================

bool setNonBlocking(socket_t fd, bool enable) {
#if defined(_WIN32)
  u_long mode = enable ? 1 : 0;
  return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  if (enable) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }
  return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

bool setKeepAlive(socket_t fd, bool enable) {
  int value = enable ? 1 : 0;
  return setsockopt(
             fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&value), sizeof(value)) ==
         0;
}

bool setKeepAliveParams(socket_t fd, int idleSec, int intervalSec, int count) {
#if defined(_WIN32)
  struct tcp_keepalive ka = {};
  ka.onoff = 1;
  ka.keepalivetime = static_cast<ULONG>(idleSec) * 1000;
  ka.keepaliveinterval = static_cast<ULONG>(intervalSec) * 1000;
  DWORD bytesReturned = 0;
  if (WSAIoctl(
          fd, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &bytesReturned, nullptr, nullptr) !=
      0) {
    return false;
  }
  (void) count;  // TCP_KEEPCNT is not configurable via WSAIoctl on Windows.
#elif defined(__linux__)
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idleSec, sizeof(idleSec)) != 0) {
    return false;
  }
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSec, sizeof(intervalSec)) != 0) {
    return false;
  }
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
    return false;
  }
#elif defined(__APPLE__)
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idleSec, sizeof(idleSec)) != 0) {
    return false;
  }
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSec, sizeof(intervalSec)) != 0) {
    return false;
  }
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
    return false;
  }
#endif
  return true;
}

bool shutdownBoth(socket_t fd) {
#if defined(_WIN32)
  return ::shutdown(fd, SD_BOTH) == 0;
#else
  return ::shutdown(fd, SHUT_RDWR) == 0;
#endif
}

void suppressSigpipe(socket_t fd) {
#if defined(__APPLE__)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void) fd;
#endif
}

// ============================================================================
// Socket I/O
// ============================================================================

ssize_t socketRecv(socket_t fd, void* data, size_t len) {
#if defined(_WIN32)
  return ::recv(fd, static_cast<char*>(data), static_cast<int>(len), 0);
#else
  return ::recv(fd, data, len, 0);
#endif
}

ssize_t socketSend(socket_t fd, const void* data, size_t len) {
#if defined(_WIN32)
  return ::send(fd, static_cast<const char*>(data), static_cast<int>(len), 0);
#elif defined(__linux__)
  return ::send(fd, data, len, MSG_NOSIGNAL);
#else
  return ::send(fd, data, len, 0);
#endif
}

// ============================================================================
// Connect helpers
// ============================================================================

int pollForWrite(socket_t fd, int timeoutMs) {
#if defined(_WIN32)
  WSAPOLLFD pfd = {};
  pfd.fd = fd;
  pfd.events = POLLOUT;
  int ret = WSAPoll(&pfd, 1, timeoutMs);
#else
  pollfd pfd = {};
  pfd.fd = fd;
  pfd.events = POLLOUT;
  int ret = ::poll(&pfd, 1, timeoutMs);
#endif

  if (ret < 0) {
    if (isInterrupted(lastSocketError())) {
      return pollForWrite(fd, timeoutMs);  // retry on EINTR
    }
    return -1;
  }
  return ret;  // 0 = timeout, >0 = ready
}

bool getSocketError(socket_t fd, int* errOut) {
  socklen_t errLen = sizeof(*errOut);
  return getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(errOut), &errLen) == 0;
}

std::string socketErrorString(int err) {
#if defined(_WIN32)
  char buf[256] = {0};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr,
                 static_cast<DWORD>(err),
                 0,
                 buf,
                 sizeof(buf),
                 nullptr);
  std::size_t len = std::strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  return buf;
#else
  return strerror(err);
#endif
}

}  // namespace chttp2
