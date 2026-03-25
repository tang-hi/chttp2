#include "chttp2/client.hpp"

#include <future>
#include <memory>
#include <string>

#include "chttp2/http2_constants.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

Client::Operation::Operation() = default;

Client::Operation::Operation(const RequestContextPtr& context) : weakContext(context) {}

bool Client::Operation::cancel() const {
  RequestContextPtr context = weakContext.lock();
  if (!context) {
    return false;
  }
  return context->cancel();
}

bool Client::Operation::isCanceled() const {
  RequestContextPtr context = weakContext.lock();
  if (!context) {
    return false;
  }
  return context->isCanceled();
}

bool Client::Operation::isValid() const {
  RequestContextPtr context = weakContext.lock();
  if (!context) {
    return false;
  }
  return !context->isFinished();
}

void Client::close() {
  connectionManager.close();
}

// ===========================================================================
// Generic send
// ===========================================================================

Response Client::Send(const Request& request) {
  std::shared_ptr<std::promise<Response>> promise = std::make_shared<std::promise<Response>>();
  std::future<Response> future = promise->get_future();

  Send(request, [promise](const Response& response) {
    try {
      promise->set_value(response);
    } catch (const std::future_error&) {  // NOLINT(bugprone-empty-catch)
      // Ignore duplicated completion.
    }
  });

  return future.get();
}

Client::Operation Client::Send(const Request& request, const CompletionHandler& callback) {
  Response invalidResponse;
  if (!validateRequest(request, &invalidResponse)) {
    if (callback) {
      callback(invalidResponse);
    }
    return Operation();
  }

  chttp2::Request wireRequest = buildWireRequest(request);
  Endpoint ep = request.endpoint;

  // Infer default port from scheme.
  if (ep.port == 0) {
    ep.port = ep.useSSL ? 443 : 80;
  }

  RequestContextPtr context =
      connectionManager.sendAsync(wireRequest, ep, request.options.timeoutMs, callback);
  return Operation(context);
}

// ===========================================================================
// Convenience methods
// ===========================================================================

Response Client::Get(const Endpoint& ep,
                     const std::string& path,
                     const Headers& headers,
                     const Options& options) {
  Request request;
  request.endpoint = ep;
  request.method = GET;
  request.target = path;
  request.headers = headers;
  request.options = options;
  return Send(request);
}

Client::Operation Client::Get(const Endpoint& ep,
                              const std::string& path,
                              const CompletionHandler& callback,
                              const Headers& headers,
                              const Options& options) {
  Request request;
  request.endpoint = ep;
  request.method = GET;
  request.target = path;
  request.headers = headers;
  request.options = options;
  return Send(request, callback);
}

Response Client::Post(const Endpoint& ep,
                      const std::string& path,
                      const std::string& body,
                      const Headers& headers,
                      const Options& options) {
  Request request;
  request.endpoint = ep;
  request.method = POST;
  request.target = path;
  request.headers = headers;
  request.body = body;
  request.options = options;
  return Send(request);
}

Client::Operation Client::Post(const Endpoint& ep,
                               const std::string& path,
                               const std::string& body,
                               const CompletionHandler& callback,
                               const Headers& headers,
                               const Options& options) {
  Request request;
  request.endpoint = ep;
  request.method = POST;
  request.target = path;
  request.headers = headers;
  request.body = body;
  request.options = options;
  return Send(request, callback);
}

// ===========================================================================
// Internal helpers
// ===========================================================================

bool Client::validateRequest(const Request& request, Response* errorResponse) const {
  const Endpoint& ep = request.endpoint;

  if (ep.host.empty() && ep.ip.empty() && ep.sockFile.empty()) {
    if (errorResponse) {
      *errorResponse = Response::customError("Request endpoint is empty");
    }
    return false;
  }

  if (request.method.empty()) {
    if (errorResponse) {
      *errorResponse = Response::customError("Request method is empty");
    }
    return false;
  }

  if (request.target.empty()) {
    if (errorResponse) {
      *errorResponse = Response::customError("Request target is empty");
    }
    return false;
  }

  if (request.target[0] != '/') {
    if (errorResponse) {
      *errorResponse = Response::customError("Request target must start with '/'");
    }
    return false;
  }

  return true;
}

chttp2::Request Client::buildWireRequest(const Request& request) const {
  chttp2::Request wire;
  const Endpoint& ep = request.endpoint;

  // Pseudo-headers.
  wire.setHeader(":method", request.method);
  wire.setHeader(":scheme", ep.useSSL ? "https" : "http");

  // :authority — host, optionally with port if non-default.
  std::string authority = ep.host.empty() ? ep.ip : ep.host;
  uint16_t defaultPort = ep.useSSL ? 443 : 80;
  if (ep.port != 0 && ep.port != defaultPort) {
    authority += ":" + std::to_string(ep.port);
  }
  wire.setHeader(":authority", authority);
  wire.setHeader(":path", request.target);

  // Default headers.
  wire.setHeader("user-agent", userAgent);
  wire.setHeader("accept", "*/*");

  // User-provided headers (may override defaults).
  for (const auto& header : request.headers) {
    wire.setHeader(header.name, header.value);
  }

  wire.body = request.body;

  if (!request.body.empty() && request.options.autoContentLength) {
    if (wire.headers.find(CONTENT_LENGTH) == wire.headers.end()) {
      wire.setHeader(CONTENT_LENGTH, std::to_string(request.body.size()));
    }
  }

  return wire;
}

}  // namespace chttp2
