#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>

#include "chttp2/client_config.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/ihttp_session.hpp"
#include "chttp2/reactor.hpp"
#include "chttp2/request.hpp"
#include "chttp2/request_context.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

using ConnectionId = uint64_t;

class Connection {
 public:
  using RequestDoneHandler = std::function<void(const RequestContextPtr&, const Response&)>;
  using ConnectionLostHandler = std::function<void(Connection*)>;
  using ConnectionDrainingHandler = std::function<void(Connection*)>;

  Connection(ConnectionId id,
             const Endpoint& ep,
             const ClientConfig& config,
             Reactor& r,
             RequestDoneHandler onRequestDone,
             ConnectionLostHandler onConnectionLost,
             ConnectionDrainingHandler onConnectionDraining);

  bool connect();
  bool registerWithReactor();

  void closeOnReactor();
  bool isConnected() const { return connected; }
  ConnectionId id() const { return connId; }
  const Endpoint& ep() const { return endpoint; }

  bool submitOnReactor(const chttp2::Request& request, const RequestContextPtr& context);
  void cancelRequestOnReactor(const RequestContextPtr& context);
  bool canAcceptNewRequests() const;
  bool hasStreamCapacity() const;
  std::size_t activeStreamCount() const;

 private:
  void onSessionReadable();
  void onSessionWritable();
  void updateWriteInterest();
  void processSessionEvents(const std::vector<SessionEvent>& events);
  void schedulePingTimer();
  void onPingTimer();
  void onPingTimeout();
  void cancelPingTimers();

  ConnectionId connId;
  Endpoint endpoint;
  Reactor& reactor;
  RequestDoneHandler requestDoneHandler;
  ConnectionLostHandler connectionLostHandler;
  ConnectionDrainingHandler connectionDrainingHandler;

  int pingIntervalSec{0};
  int pingTimeoutSec{0};

  std::unique_ptr<IHttpSession> session;
  socket_t sessionFd{INVALID_SOCKET};
  bool connected{false};
  bool pendingPing{false};
  Reactor::TimerId pingTimerId{0};
  Reactor::TimerId pingTimeoutTimerId{0};
};

}  // namespace chttp2
