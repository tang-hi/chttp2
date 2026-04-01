#include "chttp2/stream_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <openssl/err.h>
#include <stdexcept>

#include "chttp2/connector.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/http2_constants.hpp"
#include "chttp2/log.hpp"

namespace chttp2 {

bool TcpStreamTransport::init(int keepAliveSec) {
  if (!socket.valid()) {
    ready = false;
    return false;
  }

  if (!socket.setNonBlocking(true)) {
    ready = false;
    return false;
  }

  if (keepAliveSec > 0) {
    socket.setKeepAlive(true);
    int interval = std::max(keepAliveSec / 6, 1);
    socket.setKeepAliveParams(keepAliveSec, interval, 3);
  }

  ready = true;
  return true;
}

IOResult TcpStreamTransport::writeSome(const void* data, size_t len) {
  if (!ready) {
    return IOResult(IOState::ERROR, 0);
  }
  if (len == 0) {
    return IOResult(IOState::OK, 0);
  }

  ssize_t ret = socket.write(data, len);
  if (ret > 0) {
    return IOResult(IOState::OK, static_cast<size_t>(ret));
  }
  if (ret == 0) {
    return IOResult(IOState::CLOSED, 0);
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return IOResult(IOState::WOULD_BLOCK, 0);
  }
  if (errno == EPIPE || errno == ECONNRESET) {
    return IOResult(IOState::CLOSED, 0);
  }
  return IOResult(IOState::ERROR, 0);
}

IOResult TcpStreamTransport::readSome(void* data, size_t len) {
  if (!ready) {
    return IOResult(IOState::ERROR, 0);
  }
  if (len == 0) {
    return IOResult(IOState::OK, 0);
  }

  ssize_t ret = socket.read(data, len);
  if (ret > 0) {
    return IOResult(IOState::OK, static_cast<size_t>(ret));
  }
  if (ret == 0) {
    return IOResult(IOState::CLOSED, 0);
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return IOResult(IOState::WOULD_BLOCK, 0);
  }
  if (errno == EPIPE || errno == ECONNRESET) {
    return IOResult(IOState::CLOSED, 0);
  }
  return IOResult(IOState::ERROR, 0);
}

void TcpStreamTransport::close() {
  ready = false;
  if (!socket.valid()) {
    return;
  }
  socket.shutdownBoth();
  socket.reset();
}

bool TlsStreamTransport::init(int keepAliveSec) {
  if (!socket.valid()) {
    ready = false;
    return false;
  }

  if (!socket.setNonBlocking(true)) {
    ready = false;
    return false;
  }

  if (keepAliveSec > 0) {
    socket.setKeepAlive(true);
    int interval = std::max(keepAliveSec / 6, 1);
    socket.setKeepAliveParams(keepAliveSec, interval, 3);
  }

  SSL_library_init();
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();

  ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    CHTTP2_LOG_ERROR("tls: SSL_CTX_new failed");
    ready = false;
    return false;
  }

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

  if (customCert) {
    if (SSL_CTX_load_verify_locations(ctx, certFile.c_str(), nullptr) != 1) {
      CHTTP2_LOG_ERROR("tls: failed to load cert: %s", certFile.c_str());
      ready = false;
      return false;
    }
  } else {
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      CHTTP2_LOG_ERROR("tls: failed to set default verify paths");
      ready = false;
      return false;
    }
  }

  if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
    ready = false;
    return false;
  }

  std::string alpnProtocols;
  for (const auto& i : K_HTTP2_PROTOCOL) {
    alpnProtocols.push_back(static_cast<char>(i.size()));
    alpnProtocols += i;
  }

  if (SSL_CTX_set_alpn_protos(ctx,
                              reinterpret_cast<const unsigned char*>(alpnProtocols.data()),
                              static_cast<unsigned int>(alpnProtocols.size())) != 0) {
    ready = false;
    return false;
  }

  ssl = SSL_new(ctx);
  if (!ssl) {
    ready = false;
    return false;
  }

  BIO* socketBio = BIO_new(BIO_s_socket());
  if (!socketBio) {
    ready = false;
    return false;
  }

  BIO_set_fd(socketBio, socket.get(), BIO_NOCLOSE);
  BIO_set_nbio(socketBio, 1);
  SSL_set_bio(ssl, socketBio, socketBio);

  if (!domain.empty()) {
    if (SSL_set_tlsext_host_name(ssl, domain.c_str()) != 1) {
      ready = false;
      return false;
    }

    if (SSL_set1_host(ssl, domain.c_str()) != 1) {
      ready = false;
      return false;
    }
  }

  SSL_set_connect_state(ssl);
  int ret = 0;
  while ((ret = SSL_do_handshake(ssl)) <= 0) {
    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      continue;
    }
    CHTTP2_LOG_ERROR("tls: handshake failed, sslerror=%d", err);
    ready = false;
    return false;
  }

  if (SSL_get_verify_result(ssl) != X509_V_OK) {
    CHTTP2_LOG_ERROR("tls: certificate verification failed, result=%ld",
                     SSL_get_verify_result(ssl));
    ready = false;
    return false;
  }

  const unsigned char* proto = nullptr;
  unsigned int protoLen = 0;
  SSL_get0_alpn_selected(ssl, &proto, &protoLen);
  if (protoLen != K_HTTP2_PROTOCOL[0].size() ||
      std::memcmp(proto, K_HTTP2_PROTOCOL[0].data(), protoLen) != 0) {
    CHTTP2_LOG_ERROR("tls: ALPN negotiation failed, server did not select h2");
    ready = false;
    return false;
  }

  CHTTP2_LOG_DEBUG("tls: handshake complete, ALPN=h2");
  ready = true;
  return true;
}

IOResult TlsStreamTransport::writeSome(const void* data, size_t len) {
  if (!ready || !ssl) {
    return IOResult(IOState::ERROR, 0);
  }
  if (len == 0) {
    return IOResult(IOState::OK, 0);
  }

  int ret = SSL_write(ssl, data, static_cast<int>(len));
  if (ret > 0) {
    return IOResult(IOState::OK, static_cast<size_t>(ret));
  }

  int err = SSL_get_error(ssl, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return IOResult(IOState::WOULD_BLOCK, 0);
  }
  if (err == SSL_ERROR_ZERO_RETURN) {
    return IOResult(IOState::CLOSED, 0);
  }
  return IOResult(IOState::ERROR, 0);
}

IOResult TlsStreamTransport::readSome(void* data, size_t len) {
  if (!ready || !ssl) {
    return IOResult(IOState::ERROR, 0);
  }
  if (len == 0) {
    return IOResult(IOState::OK, 0);
  }

  int ret = SSL_read(ssl, data, static_cast<int>(len));
  if (ret > 0) {
    return IOResult(IOState::OK, static_cast<size_t>(ret));
  }

  int err = SSL_get_error(ssl, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return IOResult(IOState::WOULD_BLOCK, 0);
  }
  if (err == SSL_ERROR_ZERO_RETURN) {
    return IOResult(IOState::CLOSED, 0);
  }
  return IOResult(IOState::ERROR, 0);
}

void TlsStreamTransport::close() {
  ready = false;

  if (ssl) {
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ssl = nullptr;
  }

  if (ctx) {
    SSL_CTX_free(ctx);
    ctx = nullptr;
  }

  if (socket.valid()) {
    socket.shutdownBoth();
    socket.reset();
  }
}

std::unique_ptr<IStreamTransport> StreamTransportFactory::create(const Endpoint& ep,
                                                                 int connectTimeoutMs) {
  SocketFd socket;
  if (ep.type == SocketType::UNIX) {
    socket = Connector::connectUnix(ep.sockFile, connectTimeoutMs);
  } else if (!ep.ip.empty()) {
    socket = Connector::connectIp(ep.ip, ep.port, connectTimeoutMs);
  } else if (!ep.host.empty()) {
    socket = Connector::connectDomain(ep.host, ep.port, connectTimeoutMs);
  } else {
    throw std::invalid_argument("Missing endpoint for stream transport");
  }

  if (ep.useSSL) {
    return std::unique_ptr<IStreamTransport>(
        new TlsStreamTransport(std::move(socket), ep.host, ep.certFile));
  }

  return std::unique_ptr<IStreamTransport>(new TcpStreamTransport(std::move(socket)));
}

}  // namespace chttp2
