#pragma once

#include <cstddef>
#include <memory>
#include <openssl/ssl.h>
#include <string>
#include <utility>

#include "chttp2/endpoint.hpp"
#include "chttp2/socket_fd.hpp"

namespace chttp2 {

enum class IOState : uint8_t {
  OK,
  WOULD_BLOCK,
  CLOSED,
  ERR,
};

struct IOResult {
  IOState state;
  size_t bytes;

  IOResult() : state(IOState::ERR), bytes(0) {}
  IOResult(IOState ioState, size_t ioBytes) : state(ioState), bytes(ioBytes) {}
};

class IStreamTransport {
 public:
  virtual ~IStreamTransport() {}

  virtual bool init(int keepAliveSec) = 0;
  virtual bool isReady() const = 0;
  virtual IOResult writeSome(const void* data, size_t len) = 0;
  virtual IOResult readSome(void* data, size_t len) = 0;
  virtual void close() = 0;
  virtual socket_t fd() const = 0;
};

class TcpStreamTransport : public IStreamTransport {
 public:
  explicit TcpStreamTransport(SocketFd&& sock) : socket(std::move(sock)) {}

  bool init(int keepAliveSec) override;
  bool isReady() const override { return ready; }
  IOResult writeSome(const void* data, size_t len) override;
  IOResult readSome(void* data, size_t len) override;
  void close() override;
  socket_t fd() const override { return socket.get(); }

 private:
  SocketFd socket;
  bool ready{false};
};

class TlsStreamTransport : public IStreamTransport {
 public:
  TlsStreamTransport(SocketFd&& sock, std::string dom, const std::string& cert)
      : socket(std::move(sock)),
        domain(std::move(dom)),
        certFile(cert),
        customCert(!cert.empty()) {}

  ~TlsStreamTransport() override { close(); }

  bool init(int keepAliveSec) override;
  bool isReady() const override { return ready; }
  IOResult writeSome(const void* data, size_t len) override;
  IOResult readSome(void* data, size_t len) override;
  void close() override;
  socket_t fd() const override { return socket.get(); }

 private:
  SocketFd socket;
  std::string domain;
  std::string certFile;
  bool customCert{false};
  bool ready{false};
  SSL* ssl{nullptr};
  SSL_CTX* ctx{nullptr};
};

class StreamTransportFactory {
 public:
  static std::unique_ptr<IStreamTransport> create(const Endpoint& ep, int connectTimeoutMs = 0);
};

}  // namespace chttp2
