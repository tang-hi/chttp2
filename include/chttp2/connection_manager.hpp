#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chttp2/callback_dispatcher.hpp"
#include "chttp2/client_config.hpp"
#include "chttp2/connection.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/reactor.hpp"
#include "chttp2/request.hpp"
#include "chttp2/request_context.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

class ConnectionManager {
 public:
  using CompletionHandler = std::function<void(const Response&)>;

  ConnectionManager();
  explicit ConnectionManager(const ClientConfig& cfg);
  ~ConnectionManager();

  ConnectionManager(const ConnectionManager&) = delete;
  ConnectionManager& operator=(const ConnectionManager&) = delete;

  void close();

  RequestContextPtr sendAsync(const chttp2::Request& request,
                              const Endpoint& ep,
                              int timeoutMs,
                              const CompletionHandler& callback);
  bool cancel(uint64_t requestId);

 private:
  struct PendingRequest {
    chttp2::Request request;
    Endpoint endpoint;
    RequestContextPtr context;
  };

  struct ConnectTask {
    Endpoint endpoint;
    ConnectionId connId;
  };

  bool startThreadsIfNeeded();

  void connectorLoop();
  void enqueueConnectTask(const Endpoint& ep);

  void drainSubmitQueue();
  void trySubmitPending(const Endpoint& ep);
  void handleCancelOnReactor(uint64_t requestId);
  void handleTimeoutOnReactor(uint64_t requestId);
  void closeOnReactor();

  Connection* pickConnectionOnReactor(const Endpoint& ep);
  void drainPendingForEndpoint(const Endpoint& ep);

  void onConnectionEstablished(const Endpoint& ep, std::unique_ptr<Connection> conn);
  void onConnectionFailed(const Endpoint& ep);

  void onRequestDone(const RequestContextPtr& context, const Response& response);
  void onConnectionLost(Connection* conn, const Endpoint& ep);
  void onConnectionDraining(Connection* conn, const Endpoint& ep);

  void completeRequest(const RequestContextPtr& context, const Response& response);
  void failQueuedRequests(const Response& response);
  void failAllContexts(const Response& response);
  RequestContextPtr findContext(uint64_t requestId) const;
  bool removeFromSubmitQueue(uint64_t requestId, RequestContextPtr* context);

  ClientConfig config;
  Reactor reactor;
  CallbackDispatcher callbackDispatcher;
  std::atomic<uint64_t> nextRequestId{1};
  std::atomic<uint64_t> nextConnectionId{1};

  mutable std::mutex poolsMutex;
  // A vector per endpoint (rather than a single unique_ptr) so that during a
  // GOAWAY transition the old draining connection and the new connection can
  // coexist.  The old one stays alive until its in-flight streams finish, then
  // gets pruned in drainSubmitQueue.
  std::unordered_map<Endpoint, std::vector<std::unique_ptr<Connection>>, EndpointHash> pools;
  std::unordered_map<Endpoint, std::deque<PendingRequest>, EndpointHash> pendingByEndpoint;
  std::unordered_set<Endpoint, EndpointHash> connectingEndpoints;
  std::unordered_map<uint64_t, Connection*> requestConnections;

  // Ensures startThreadsIfNeeded runs exactly once.
  std::once_flag startOnce;
  bool startResult{false};

  // Connector thread.
  std::thread connectorThread;
  std::mutex connectorMutex;
  std::condition_variable connectorCv;
  std::deque<ConnectTask> connectTasks;
  bool connectorStop{false};

  // Request queue (user thread -> reactor thread).
  mutable std::mutex submitMutex;
  std::deque<PendingRequest> submitQueue;

  // All outstanding request contexts.
  mutable std::mutex contextsMutex;
  std::unordered_map<uint64_t, RequestContextPtr> contexts;
};

}  // namespace chttp2
