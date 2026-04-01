#include "chttp2/connector.hpp"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "chttp2/log.hpp"

namespace chttp2 {

namespace {

void suppressSigpipe(socket_t fd) {
#if defined(__APPLE__)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void) fd;
#endif
}

bool makeNonBlocking(socket_t fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Non-blocking connect with poll-based timeout.
// The socket must already be in non-blocking mode.
// timeoutMs <= 0 means wait indefinitely.
bool connectWithTimeout(socket_t fd, const sockaddr* addr, socklen_t addrLen, int timeoutMs) {
  int ret = ::connect(fd, addr, addrLen);
  if (ret == 0) {
    return true;
  }
  if (errno != EINPROGRESS) {
    return false;
  }

  using Clock = std::chrono::steady_clock;
  auto deadline = (timeoutMs > 0) ? Clock::now() + std::chrono::milliseconds(timeoutMs)
                                  : Clock::time_point::max();

  while (true) {
    int pollMs = -1;
    if (timeoutMs > 0) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
      if (left.count() <= 0) {
        errno = ETIMEDOUT;
        return false;
      }
      pollMs = static_cast<int>(left.count());
    }

    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int pollRet = ::poll(&pfd, 1, pollMs);
    if (pollRet < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (pollRet == 0) {
      errno = ETIMEDOUT;
      return false;
    }

    int sockErr = 0;
    socklen_t errLen = sizeof(sockErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockErr, &errLen) != 0) {
      return false;
    }
    if (sockErr != 0) {
      errno = sockErr;
      return false;
    }
    return true;
  }
}

}  // namespace

SocketFd Connector::connectIp(const std::string& ip, uint16_t port, int timeoutMs) {
  CHTTP2_LOG_DEBUG("connecting to %s:%u", ip.c_str(), port);

  addrinfo hints = {};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;

  std::string portStr = std::to_string(port);
  addrinfo* result = nullptr;
  int gaiErr = getaddrinfo(ip.c_str(), portStr.c_str(), &hints, &result);
  if (gaiErr != 0 || !result) {
    CHTTP2_LOG_ERROR(
        "connect %s:%u failed: invalid IP: %s", ip.c_str(), port, gai_strerror(gaiErr));
    throw std::runtime_error("Invalid IP address");
  }

  socket_t fd = socket(result->ai_family, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    freeaddrinfo(result);
    CHTTP2_LOG_ERROR("connect %s:%u failed: socket creation", ip.c_str(), port);
    throw std::runtime_error("Failed to create socket");
  }

  suppressSigpipe(fd);
  makeNonBlocking(fd);

  bool ok = connectWithTimeout(fd, result->ai_addr, result->ai_addrlen, timeoutMs);
  int err = errno;
  freeaddrinfo(result);

  if (!ok) {
    ::close(fd);
    CHTTP2_LOG_ERROR("connect %s:%u failed: %s", ip.c_str(), port, strerror(err));
    throw std::runtime_error(err == ETIMEDOUT ? "Connect timeout" : "Failed to connect");
  }

  CHTTP2_LOG_INFO("connected to %s:%u fd=%d", ip.c_str(), port, fd);
  return SocketFd(fd);
}

SocketFd Connector::connectDomain(const std::string& domain, uint16_t port, int timeoutMs) {
  CHTTP2_LOG_DEBUG("resolving %s:%u", domain.c_str(), port);

  addrinfo hints = {};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;

  std::string portStr = std::to_string(port);
  addrinfo* result = nullptr;
  int gaiErr = getaddrinfo(domain.c_str(), portStr.c_str(), &hints, &result);
  if (gaiErr != 0 || !result) {
    CHTTP2_LOG_ERROR(
        "connect %s:%u failed: DNS resolution: %s", domain.c_str(), port, gai_strerror(gaiErr));
    throw std::runtime_error("Failed to resolve domain");
  }

  using Clock = std::chrono::steady_clock;
  auto deadline = (timeoutMs > 0) ? Clock::now() + std::chrono::milliseconds(timeoutMs)
                                  : Clock::time_point::max();

  std::string lastError;
  for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    int remaining = -1;
    if (timeoutMs > 0) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
      if (left.count() <= 0) {
        lastError = "Connect timeout";
        break;
      }
      remaining = static_cast<int>(left.count());
    }

    socket_t fd = socket(rp->ai_family, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
      continue;
    }

    suppressSigpipe(fd);
    makeNonBlocking(fd);

    if (connectWithTimeout(fd, rp->ai_addr, rp->ai_addrlen, remaining)) {
      freeaddrinfo(result);
      CHTTP2_LOG_INFO("connected to %s:%u fd=%d", domain.c_str(), port, fd);
      return SocketFd(fd);
    }

    lastError = strerror(errno);
    ::close(fd);
  }

  freeaddrinfo(result);
  CHTTP2_LOG_ERROR("connect %s:%u failed: %s", domain.c_str(), port, lastError.c_str());
  throw std::runtime_error("Failed to connect: " + lastError);
}

SocketFd Connector::connectUnix(const std::string& sockFile, int timeoutMs) {
  CHTTP2_LOG_DEBUG("connecting to unix:%s", sockFile.c_str());

  socket_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    CHTTP2_LOG_ERROR("connect unix:%s failed: socket creation", sockFile.c_str());
    throw std::runtime_error("Failed to create unix socket");
  }

  suppressSigpipe(fd);
  makeNonBlocking(fd);

  sockaddr_un serverAddr = {};
  serverAddr.sun_family = AF_UNIX;
  strncpy(serverAddr.sun_path, sockFile.c_str(), sizeof(serverAddr.sun_path) - 1);

  if (!connectWithTimeout(
          fd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr), timeoutMs)) {
    int err = errno;
    ::close(fd);
    CHTTP2_LOG_ERROR("connect unix:%s failed: %s", sockFile.c_str(), strerror(err));
    throw std::runtime_error(err == ETIMEDOUT ? "Connect timeout"
                                              : "Failed to connect unix socket");
  }

  CHTTP2_LOG_INFO("connected to unix:%s fd=%d", sockFile.c_str(), fd);
  return SocketFd(fd);
}

}  // namespace chttp2
