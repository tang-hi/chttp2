#pragma once

#include <cstdint>
#include <string>

#include "chttp2/socket_fd.hpp"

namespace chttp2 {

class Connector {
 public:
  static SocketFd connectIp(const std::string& ip, uint16_t port, int timeoutMs = 0);
  static SocketFd connectDomain(const std::string& domain, uint16_t port, int timeoutMs = 0);
  static SocketFd connectUnix(const std::string& sockFile, int timeoutMs = 0);
};

}  // namespace chttp2
