#include "chttp2/connector.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

#include "chttp2/log.hpp"
#include "chttp2/platform.hpp"

#if !defined(_WIN32)
#include <sys/un.h>
#endif

namespace chttp2 {

namespace {

// Non-blocking connect with poll-based timeout.
// The socket must already be in non-blocking mode.
// timeoutMs <= 0 means wait indefinitely.
bool connectWithTimeout(socket_t fd, const sockaddr* addr, socklen_t addrLen, int timeoutMs) {
  int ret = ::connect(fd, addr, addrLen);
  if (ret == 0) {
    return true;
  }
  if (!isInProgress(lastSocketError())) {
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
        setLastSocketError(SOCKET_TIMED_OUT);
        return false;
      }
      pollMs = static_cast<int>(left.count());
    }

    int pollRet = pollForWrite(fd, pollMs);
    if (pollRet < 0) {
      return false;
    }
    if (pollRet == 0) {
      setLastSocketError(SOCKET_TIMED_OUT);
      return false;
    }

    int sockErr = 0;
    if (!getSocketError(fd, &sockErr)) {
      return false;
    }
    if (sockErr != 0) {
      setLastSocketError(sockErr);
      return false;
    }
    return true;
  }
}

}  // namespace

SocketFd Connector::connectIp(const std::string& ip, uint16_t port, int timeoutMs) {
  CHTTP2_LOG_DEBUG("connecting to %s:%u", ip.c_str(), port);

  ensureWinsockInitialized();

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
  chttp2::setNonBlocking(fd, true);

  bool ok = connectWithTimeout(fd, result->ai_addr, result->ai_addrlen, timeoutMs);
  int err = lastSocketError();
  freeaddrinfo(result);

  if (!ok) {
    closeSocket(fd);
    CHTTP2_LOG_ERROR("connect %s:%u failed: %s", ip.c_str(), port, socketErrorString(err).c_str());
    throw std::runtime_error(isTimedOut(err) ? "Connect timeout" : "Failed to connect");
  }

  CHTTP2_LOG_INFO("connected to %s:%u fd=%d", ip.c_str(), port, fdToInt(fd));
  return SocketFd(fd);
}

SocketFd Connector::connectDomain(const std::string& domain, uint16_t port, int timeoutMs) {
  CHTTP2_LOG_DEBUG("resolving %s:%u", domain.c_str(), port);

  ensureWinsockInitialized();

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
    chttp2::setNonBlocking(fd, true);

    if (connectWithTimeout(fd, rp->ai_addr, rp->ai_addrlen, remaining)) {
      freeaddrinfo(result);
      CHTTP2_LOG_INFO("connected to %s:%u fd=%d", domain.c_str(), port, fdToInt(fd));
      return SocketFd(fd);
    }

    lastError = socketErrorString(lastSocketError());
    closeSocket(fd);
  }

  freeaddrinfo(result);
  CHTTP2_LOG_ERROR("connect %s:%u failed: %s", domain.c_str(), port, lastError.c_str());
  throw std::runtime_error("Failed to connect: " + lastError);
}

SocketFd Connector::connectUnix(const std::string& sockFile, int timeoutMs) {
#if !defined(_WIN32)
  CHTTP2_LOG_DEBUG("connecting to unix:%s", sockFile.c_str());

  socket_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    CHTTP2_LOG_ERROR("connect unix:%s failed: socket creation", sockFile.c_str());
    throw std::runtime_error("Failed to create unix socket");
  }

  suppressSigpipe(fd);
  chttp2::setNonBlocking(fd, true);

  sockaddr_un serverAddr = {};
  serverAddr.sun_family = AF_UNIX;
  strncpy(serverAddr.sun_path, sockFile.c_str(), sizeof(serverAddr.sun_path) - 1);

  if (!connectWithTimeout(
          fd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr), timeoutMs)) {
    int err = lastSocketError();
    closeSocket(fd);
    CHTTP2_LOG_ERROR(
        "connect unix:%s failed: %s", sockFile.c_str(), socketErrorString(err).c_str());
    throw std::runtime_error(isTimedOut(err) ? "Connect timeout" : "Failed to connect unix socket");
  }

  CHTTP2_LOG_INFO("connected to unix:%s fd=%d", sockFile.c_str(), fd);
  return SocketFd(fd);
#else
  (void) timeoutMs;
  CHTTP2_LOG_ERROR("connect unix:%s failed: not supported on Windows", sockFile.c_str());
  throw std::runtime_error("Unix sockets are not supported on Windows");
#endif
}

}  // namespace chttp2
