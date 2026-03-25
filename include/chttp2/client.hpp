#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chttp2/client_config.hpp"
#include "chttp2/connection_manager.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/header_field.hpp"
#include "chttp2/request.hpp"
#include "chttp2/request_context.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

class Client {
 public:
  using CompletionHandler = std::function<void(const Response&)>;
  using Headers = std::vector<HeaderField>;

  struct Options {
    // Negative means no timeout.
    Options() : timeoutMs(-1), autoContentLength(true) {}
    Options(const Options&) = default;
    Options(Options&&) = default;
    Options& operator=(const Options&) = default;
    Options& operator=(Options&&) = default;
    int timeoutMs;
    bool autoContentLength;
  };

  struct Request {
    Endpoint endpoint;
    std::string method{"GET"};
    std::string target{"/"};
    Headers headers;
    std::string body;
    Options options;
  };

  class Operation {
   public:
    Operation();
    bool cancel() const;
    bool isCanceled() const;
    bool isValid() const;

   private:
    explicit Operation(const RequestContextPtr& context);
    std::weak_ptr<RequestContext> weakContext;

    friend class Client;
  };

  Client() = default;
  explicit Client(const ClientConfig& config) : connectionManager(config) {}

  ~Client() { close(); }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void close();

  // ---- Generic send ----
  Response Send(const Request& request);  // NOLINT(readability-identifier-naming)
  Operation Send(const Request& request,  // NOLINT(readability-identifier-naming)
                 const CompletionHandler& callback);

  // ---- Convenience methods ----
  Response Get(const Endpoint& ep,  // NOLINT(readability-identifier-naming)
               const std::string& path,
               const Headers& headers = Headers(),
               const Options& options = Options());

  Operation Get(const Endpoint& ep,  // NOLINT(readability-identifier-naming)
                const std::string& path,
                const CompletionHandler& callback,
                const Headers& headers = Headers(),
                const Options& options = Options());

  Response Post(const Endpoint& ep,  // NOLINT(readability-identifier-naming)
                const std::string& path,
                const std::string& body,
                const Headers& headers = Headers(),
                const Options& options = Options());

  Operation Post(const Endpoint& ep,  // NOLINT(readability-identifier-naming)
                 const std::string& path,
                 const std::string& body,
                 const CompletionHandler& callback,
                 const Headers& headers = Headers(),
                 const Options& options = Options());

 private:
  bool validateRequest(const Request& request, Response* errorResponse) const;
  chttp2::Request buildWireRequest(const Request& request) const;

  ConnectionManager connectionManager;
  std::string userAgent{"chttp2/0.1"};
};

}  // namespace chttp2
