#include "chttp2/connector.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "chttp2/log.hpp"

namespace chttp2 {

SocketFd Connector::connectIp(const std::string& ip, uint16_t port) {
  CHTTP2_LOG_DEBUG("connecting to %s:%u", ip.c_str(), port);
  socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    CHTTP2_LOG_ERROR("connect %s:%u failed: socket creation", ip.c_str(), port);
    throw std::runtime_error("Failed to create socket");
  }

  sockaddr_in serverAddr;
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0) {
    ::close(fd);
    CHTTP2_LOG_ERROR("connect %s:%u failed: invalid IP", ip.c_str(), port);
    throw std::runtime_error("Invalid IP address");
  }

  if (connect(fd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) != 0) {
    ::close(fd);
    CHTTP2_LOG_ERROR("connect %s:%u failed: %s", ip.c_str(), port, strerror(errno));
    throw std::runtime_error("Failed to connect");
  }

  CHTTP2_LOG_INFO("connected to %s:%u fd=%d", ip.c_str(), port, fd);
  return SocketFd(fd);
}

SocketFd Connector::connectDomain(const std::string& domain, uint16_t port) {
  CHTTP2_LOG_DEBUG("resolving %s:%u", domain.c_str(), port);
  hostent* host = gethostbyname(domain.c_str());
  if (host == nullptr) {
    CHTTP2_LOG_ERROR("connect %s:%u failed: DNS resolution", domain.c_str(), port);
    throw std::runtime_error("Failed to resolve domain");
  }

  socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    CHTTP2_LOG_ERROR("connect %s:%u failed: socket creation", domain.c_str(), port);
    throw std::runtime_error("Failed to create socket");
  }

  sockaddr_in serverAddr;
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(port);
  memcpy(reinterpret_cast<char*>(&serverAddr.sin_addr.s_addr),
         host->h_addr,
         static_cast<size_t>(host->h_length));

  if (connect(fd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) != 0) {
    ::close(fd);
    CHTTP2_LOG_ERROR("connect %s:%u failed: %s", domain.c_str(), port, strerror(errno));
    throw std::runtime_error("Failed to connect");
  }

  CHTTP2_LOG_INFO("connected to %s:%u fd=%d", domain.c_str(), port, fd);
  return SocketFd(fd);
}

SocketFd Connector::connectUnix(const std::string& sockFile) {
  CHTTP2_LOG_DEBUG("connecting to unix:%s", sockFile.c_str());
  socket_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
    CHTTP2_LOG_ERROR("connect unix:%s failed: socket creation", sockFile.c_str());
    throw std::runtime_error("Failed to create unix socket");
  }

  sockaddr_un serverAddr;
  serverAddr.sun_family = AF_UNIX;
  strncpy(serverAddr.sun_path, sockFile.c_str(), sizeof(serverAddr.sun_path) - 1);
  serverAddr.sun_path[sizeof(serverAddr.sun_path) - 1] = '\0';

  if (connect(fd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) != 0) {
    ::close(fd);
    CHTTP2_LOG_ERROR("connect unix:%s failed: %s", sockFile.c_str(), strerror(errno));
    throw std::runtime_error("Failed to connect unix socket");
  }

  CHTTP2_LOG_INFO("connected to unix:%s fd=%d", sockFile.c_str(), fd);
  return SocketFd(fd);
}

}  // namespace chttp2
