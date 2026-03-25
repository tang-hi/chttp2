#include "chttp2/connection_manager.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "chttp2/log.hpp"

namespace chttp2 {

ConnectionManager::ConnectionManager() = default;

ConnectionManager::ConnectionManager(const ClientConfig& cfg) : config(cfg) {}

ConnectionManager::~ConnectionManager() {
  close();
}

// ===========================================================================
// User-thread API
// ===========================================================================

bool ConnectionManager::startThreadsIfNeeded() {
  std::call_once(startOnce, [this]() {
    if (!callbackDispatcher.start()) {
      CHTTP2_LOG_ERROR("failed to start callback dispatcher");
      return;
    }

    if (!reactor.start()) {
      CHTTP2_LOG_ERROR("failed to start reactor");
      callbackDispatcher.stop();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(connectorMutex);
      connectorStop = false;
    }
    connectorThread = std::thread(&ConnectionManager::connectorLoop, this);
    startResult = true;
  });
  return startResult;
}

void ConnectionManager::close() {
  CHTTP2_LOG_INFO("closing connection manager");

  {
    std::lock_guard<std::mutex> lock(connectorMutex);
    connectorStop = true;
  }
  connectorCv.notify_one();
  if (connectorThread.joinable()) {
    connectorThread.join();
  }

  if (reactor.isRunning()) {
    reactor.post([this]() { closeOnReactor(); });
  }

  reactor.stop();
  callbackDispatcher.stop();
  failQueuedRequests(Response::failConnectionError("Connection closed"));
  failAllContexts(Response::failConnectionError("Connection closed"));
}

RequestContextPtr ConnectionManager::sendAsync(const chttp2::Request& request,
                                               const Endpoint& ep,
                                               int timeoutMs,
                                               const CompletionHandler& callback) {
  uint64_t requestId = nextRequestId.fetch_add(1);
  CHTTP2_LOG_DEBUG(
      "send_async req=%zu to %s:%u timeout=%dms", requestId, ep.host.c_str(), ep.port, timeoutMs);
  RequestContextPtr context = std::make_shared<RequestContext>(requestId, timeoutMs, callback);
  context->setCancelHandler([this](uint64_t id) { cancel(id); });

  if (!startThreadsIfNeeded()) {
    CHTTP2_LOG_WARN("send_async req=%zu failed: could not start", requestId);
    completeRequest(context, Response::failConnectionError("Connection manager failed to start"));
    return context;
  }

  {
    std::lock_guard<std::mutex> lock(contextsMutex);
    contexts[requestId] = context;
  }

  if (timeoutMs >= 0) {
    Reactor::TimerId timerId =
        reactor.scheduleOnce(timeoutMs, [this, requestId]() { handleTimeoutOnReactor(requestId); });
    if (timerId == 0) {
      completeRequest(context, Response::failConnectionError("Failed to schedule timeout"));
      return context;
    }
    context->setTimerId(timerId);
  }

  {
    std::lock_guard<std::mutex> lock(submitMutex);
    PendingRequest pending;
    pending.request = request;
    pending.endpoint = ep;
    pending.context = context;
    submitQueue.push_back(std::move(pending));
  }

  bool posted = reactor.post([this]() { drainSubmitQueue(); });
  if (!posted) {
    CHTTP2_LOG_ERROR("send_async req=%zu failed: reactor not running", requestId);
    RequestContextPtr queued;
    removeFromSubmitQueue(requestId, &queued);
    completeRequest(context, Response::failConnectionError("Reactor is not running"));
  }

  return context;
}

bool ConnectionManager::cancel(uint64_t requestId) {
  CHTTP2_LOG_DEBUG("cancel req=%zu", requestId);
  RequestContextPtr context = findContext(requestId);
  if (!context || context->isFinished()) {
    return false;
  }

  context->markCanceled();

  bool posted = reactor.post([this, requestId]() { handleCancelOnReactor(requestId); });
  if (!posted) {
    handleCancelOnReactor(requestId);
  }
  return true;
}

// ===========================================================================
// Connector thread
// ===========================================================================

void ConnectionManager::connectorLoop() {
  while (true) {
    ConnectTask task;
    {
      std::unique_lock<std::mutex> lock(connectorMutex);
      connectorCv.wait(lock, [this]() { return connectorStop || !connectTasks.empty(); });
      if (connectorStop && connectTasks.empty()) {
        return;
      }
      task = std::move(connectTasks.front());
      connectTasks.pop_front();
    }

    CHTTP2_LOG_INFO(
        "connector: connecting to %s:%u", task.endpoint.host.c_str(), task.endpoint.port);

    Endpoint ep = task.endpoint;
    auto holder = std::make_shared<ConnectionHolder>();
    holder->conn = std::make_unique<Connection>(
        task.connId,
        ep,
        config,
        reactor,
        [this](const RequestContextPtr& ctx, const Response& resp) { onRequestDone(ctx, resp); },
        [this, ep](Connection* c) { onConnectionLost(c, ep); },
        [this, ep](Connection* c) { onConnectionDraining(c, ep); });

    if (holder->conn->connect()) {
      reactor.post([this, ep, holder]() { onConnectionEstablished(ep, std::move(holder->conn)); });
    } else {
      CHTTP2_LOG_ERROR("connector: failed to connect to %s:%u", ep.host.c_str(), ep.port);
      reactor.post([this, ep]() { onConnectionFailed(ep); });
    }
  }
}

void ConnectionManager::enqueueConnectTask(const Endpoint& ep) {
  if (connectingEndpoints.count(ep)) {
    return;
  }

  if (pickConnectionOnReactor(ep)) {
    return;
  }

  connectingEndpoints.insert(ep);

  ConnectTask task;
  task.endpoint = ep;
  task.connId = nextConnectionId.fetch_add(1);

  {
    std::lock_guard<std::mutex> lock(connectorMutex);
    connectTasks.push_back(std::move(task));
  }
  connectorCv.notify_one();
}

// ===========================================================================
// Reactor-thread: connection established / failed
// ===========================================================================

void ConnectionManager::onConnectionEstablished(const Endpoint& ep,
                                                std::unique_ptr<Connection> conn) {
  connectingEndpoints.erase(ep);

  Connection* raw = conn.get();
  if (!raw->registerWithReactor()) {
    CHTTP2_LOG_ERROR("failed to register connection %zu", raw->id());

    drainPendingForEndpoint(ep);
    return;
  }

  CHTTP2_LOG_INFO("connection %zu established for %s:%u", raw->id(), ep.host.c_str(), ep.port);

  {
    std::lock_guard<std::mutex> lock(poolsMutex);
    pools[ep].push_back(std::move(conn));
  }

  auto it = pendingByEndpoint.find(ep);
  if (it == pendingByEndpoint.end()) {
    return;
  }

  std::deque<PendingRequest> pending;
  pending.swap(it->second);
  pendingByEndpoint.erase(it);

  for (auto& pr : pending) {
    if (!pr.context || pr.context->isCanceled()) {
      completeRequest(pr.context, Response::customError("Request canceled"));
      continue;
    }
    if (raw->hasStreamCapacity()) {
      if (raw->submitOnReactor(pr.request, pr.context)) {
        requestConnections[pr.context->requestId()] = raw;
      } else if (!pr.context->isFinished()) {
        completeRequest(pr.context, Response::failConnectionError("Submit failed"));
      }
    } else {
      // Connection full — put back for later.
      pendingByEndpoint[ep].push_back(std::move(pr));
    }
  }
}

void ConnectionManager::onConnectionFailed(const Endpoint& ep) {
  connectingEndpoints.erase(ep);

  drainPendingForEndpoint(ep);
}

void ConnectionManager::drainPendingForEndpoint(const Endpoint& ep) {
  auto it = pendingByEndpoint.find(ep);
  if (it == pendingByEndpoint.end()) {
    return;
  }

  std::deque<PendingRequest> pending;
  pending.swap(it->second);
  pendingByEndpoint.erase(it);

  for (auto& pr : pending) {
    completeRequest(pr.context, Response::failConnectionError("Connection failed"));
  }
}

// ===========================================================================
// Reactor-thread: request processing
// ===========================================================================

void ConnectionManager::drainSubmitQueue() {
  {
    std::lock_guard<std::mutex> lock(poolsMutex);
    for (auto& kv : pools) {
      auto& conns = kv.second;
      conns.erase(std::remove_if(
                      conns.begin(),
                      conns.end(),
                      [](const std::unique_ptr<Connection>& c) { return !c || !c->isConnected(); }),
                  conns.end());
    }
  }

  std::deque<PendingRequest> localQueue;
  {
    std::lock_guard<std::mutex> lock(submitMutex);
    localQueue.swap(submitQueue);
  }

  for (auto& pending : localQueue) {
    RequestContextPtr context = pending.context;
    if (!context) {
      continue;
    }

    if (context->isCanceled()) {
      CHTTP2_LOG_DEBUG("drain_submit req=%zu already canceled", context->requestId());
      completeRequest(context, Response::customError("Request canceled"));
      continue;
    }

    Connection* conn = pickConnectionOnReactor(pending.endpoint);
    if (conn && conn->hasStreamCapacity()) {
      if (conn->submitOnReactor(pending.request, context)) {
        requestConnections[context->requestId()] = conn;
      } else if (!context->isFinished()) {
        CHTTP2_LOG_ERROR("drain_submit req=%zu submit failed", context->requestId());
        completeRequest(context, Response::failConnectionError("Submit failed"));
      }
    } else if (conn) {
      // Connection exists but at stream limit — queue for retry.
      Endpoint ep = pending.endpoint;
      pendingByEndpoint[ep].push_back(std::move(pending));
    } else {
      Endpoint ep = pending.endpoint;
      pendingByEndpoint[ep].push_back(std::move(pending));
      enqueueConnectTask(ep);
    }
  }
}

Connection* ConnectionManager::pickConnectionOnReactor(const Endpoint& ep) {
  Connection* best = nullptr;
  std::size_t bestLoad = SIZE_MAX;

  std::lock_guard<std::mutex> lock(poolsMutex);
  auto it = pools.find(ep);
  if (it == pools.end()) {
    return nullptr;
  }

  for (const auto& conn : it->second) {
    if (conn && conn->canAcceptNewRequests()) {
      std::size_t load = conn->activeStreamCount();
      if (load < bestLoad) {
        best = conn.get();
        bestLoad = load;
      }
    }
  }
  return best;
}

void ConnectionManager::handleCancelOnReactor(uint64_t requestId) {
  RequestContextPtr context = findContext(requestId);
  if (!context || context->isFinished()) {
    return;
  }

  if (!context->isCanceled()) {
    context->markCanceled();
  }

  RequestContextPtr queuedContext;
  if (removeFromSubmitQueue(requestId, &queuedContext)) {
    CHTTP2_LOG_DEBUG("cancel req=%zu removed from submit queue", requestId);
    completeRequest(queuedContext ? queuedContext : context,
                    Response::customError("Request canceled"));
    return;
  }

  for (auto& kv : pendingByEndpoint) {
    auto& deq = kv.second;
    for (auto it = deq.begin(); it != deq.end(); ++it) {
      if (it->context && it->context->requestId() == requestId) {
        completeRequest(it->context, Response::customError("Request canceled"));
        deq.erase(it);
        return;
      }
    }
  }

  auto it = requestConnections.find(requestId);
  if (it != requestConnections.end() && it->second->isConnected()) {
    it->second->cancelRequestOnReactor(context);
  }

  if (!context->isFinished()) {
    completeRequest(context, Response::customError("Request canceled"));
  }
}

void ConnectionManager::handleTimeoutOnReactor(uint64_t requestId) {
  RequestContextPtr context = findContext(requestId);
  if (!context || context->isFinished()) {
    return;
  }

  CHTTP2_LOG_DEBUG("timeout req=%zu", requestId);
  context->markCanceled();

  RequestContextPtr queuedContext;
  if (removeFromSubmitQueue(requestId, &queuedContext)) {
    completeRequest(queuedContext ? queuedContext : context,
                    Response::customError("Request timeout"));
    return;
  }

  for (auto& kv : pendingByEndpoint) {
    auto& deq = kv.second;
    for (auto it = deq.begin(); it != deq.end(); ++it) {
      if (it->context && it->context->requestId() == requestId) {
        completeRequest(it->context, Response::customError("Request timeout"));
        deq.erase(it);
        return;
      }
    }
  }

  auto it = requestConnections.find(requestId);
  if (it != requestConnections.end() && it->second->isConnected()) {
    it->second->cancelRequestOnReactor(context);
  }

  if (!context->isFinished()) {
    completeRequest(context, Response::customError("Request timeout"));
  }
}

void ConnectionManager::closeOnReactor() {
  for (auto& kv : pendingByEndpoint) {
    for (auto& pr : kv.second) {
      completeRequest(pr.context, Response::failConnectionError("Connection closed"));
    }
  }
  pendingByEndpoint.clear();
  connectingEndpoints.clear();

  std::unordered_map<Endpoint, std::vector<std::unique_ptr<Connection>>, EndpointHash> local;
  {
    std::lock_guard<std::mutex> lock(poolsMutex);
    local.swap(pools);
  }

  for (auto& kv : local) {
    for (auto& conn : kv.second) {
      if (conn && conn->isConnected()) {
        conn->closeOnReactor();
      }
    }
  }

  requestConnections.clear();
}

// ===========================================================================
// Connection event callbacks (reactor thread)
// ===========================================================================

void ConnectionManager::onRequestDone(const RequestContextPtr& context, const Response& response) {
  Connection* conn = nullptr;
  if (context) {
    auto it = requestConnections.find(context->requestId());
    if (it != requestConnections.end()) {
      conn = it->second;
      requestConnections.erase(it);
    }
  }
  completeRequest(context, response);

  // A stream slot just freed up — try to submit pending requests.
  if (conn) {
    trySubmitPending(conn->ep());
  }
}

void ConnectionManager::trySubmitPending(const Endpoint& ep) {
  auto it = pendingByEndpoint.find(ep);
  if (it == pendingByEndpoint.end() || it->second.empty()) {
    return;
  }

  Connection* conn = pickConnectionOnReactor(ep);
  if (!conn) {
    return;
  }

  auto& queue = it->second;
  while (!queue.empty() && conn->hasStreamCapacity()) {
    PendingRequest pr = std::move(queue.front());
    queue.pop_front();

    if (!pr.context || pr.context->isCanceled()) {
      completeRequest(pr.context, Response::customError("Request canceled"));
      continue;
    }

    if (conn->submitOnReactor(pr.request, pr.context)) {
      requestConnections[pr.context->requestId()] = conn;
    } else if (!pr.context->isFinished()) {
      completeRequest(pr.context, Response::failConnectionError("Submit failed"));
    }
  }

  if (queue.empty()) {
    pendingByEndpoint.erase(it);
  }
}

void ConnectionManager::onConnectionLost(Connection* conn, const Endpoint& ep) {
  CHTTP2_LOG_WARN("connection %zu lost (%s:%u)", conn->id(), ep.host.c_str(), ep.port);

  for (auto it = requestConnections.begin(); it != requestConnections.end();) {
    if (it->second == conn) {
      it = requestConnections.erase(it);
    } else {
      ++it;
    }
  }
}

void ConnectionManager::onConnectionDraining(Connection* conn, const Endpoint& ep) {
  CHTTP2_LOG_WARN("connection %zu draining (%s:%u)", conn->id(), ep.host.c_str(), ep.port);
}

// ===========================================================================
// Shared helpers
// ===========================================================================

void ConnectionManager::completeRequest(const RequestContextPtr& context,
                                        const Response& response) {
  if (!context) {
    return;
  }

  if (!context->tryFinish()) {
    return;
  }

  CHTTP2_LOG_DEBUG("complete req=%zu isError=%d %s",
                   context->requestId(),
                   response.isError,
                   response.isError ? response.errorMsg.c_str() : "");

  uint64_t timerId = context->getTimerId();
  if (timerId != 0) {
    reactor.cancelTimer(static_cast<Reactor::TimerId>(timerId));
    context->setTimerId(0);
  }

  {
    std::lock_guard<std::mutex> lock(contextsMutex);
    contexts.erase(context->requestId());
  }

  if (callbackDispatcher.isRunning()) {
    const Response& responseCopy = response;
    bool posted = callbackDispatcher.post(
        [context, responseCopy]() { context->invokeCompletion(responseCopy); });
    if (posted) {
      return;
    }
  }

  context->invokeCompletion(response);
}

void ConnectionManager::failQueuedRequests(const Response& response) {
  std::deque<PendingRequest> localQueue;
  {
    std::lock_guard<std::mutex> lock(submitMutex);
    localQueue.swap(submitQueue);
  }

  if (!localQueue.empty()) {
    CHTTP2_LOG_WARN(
        "failing %zu queued requests: %s", localQueue.size(), response.errorMsg.c_str());
  }
  for (auto& pending : localQueue) {
    completeRequest(pending.context, response);
  }
}

void ConnectionManager::failAllContexts(const Response& response) {
  std::vector<RequestContextPtr> localContexts;
  {
    std::lock_guard<std::mutex> lock(contextsMutex);
    for (auto& pair : contexts) {
      localContexts.push_back(pair.second);
    }
  }

  if (!localContexts.empty()) {
    CHTTP2_LOG_WARN(
        "failing %zu inflight requests: %s", localContexts.size(), response.errorMsg.c_str());
  }
  for (auto& ctx : localContexts) {
    completeRequest(ctx, response);
  }
}

RequestContextPtr ConnectionManager::findContext(uint64_t requestId) const {
  std::lock_guard<std::mutex> lock(contextsMutex);
  auto it = contexts.find(requestId);
  if (it == contexts.end()) {
    return RequestContextPtr();
  }
  return it->second;
}

bool ConnectionManager::removeFromSubmitQueue(uint64_t requestId, RequestContextPtr* context) {
  std::lock_guard<std::mutex> lock(submitMutex);
  for (auto it = submitQueue.begin(); it != submitQueue.end(); ++it) {
    if (!it->context || it->context->requestId() != requestId) {
      continue;
    }

    if (context) {
      *context = it->context;
    }
    submitQueue.erase(it);
    return true;
  }
  return false;
}

}  // namespace chttp2
