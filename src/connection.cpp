#include "chttp2/connection.hpp"

#include <vector>

#include "chttp2/http2_session.hpp"
#include "chttp2/log.hpp"

namespace chttp2 {

#ifndef NDEBUG
#define CHTTP2_ASSERT_REACTOR_THREAD() assert(std::this_thread::get_id() == reactor.threadId())
#else
#define CHTTP2_ASSERT_REACTOR_THREAD() ((void) 0)
#endif

Connection::Connection(ConnectionId id,
                       const Endpoint& ep,
                       const ClientConfig& config,
                       Reactor& r,
                       RequestDoneHandler onRequestDone,
                       ConnectionLostHandler onConnectionLost,
                       ConnectionDrainingHandler onConnectionDraining)
    : connId(id),
      endpoint(ep),
      reactor(r),
      requestDoneHandler(std::move(onRequestDone)),
      connectionLostHandler(std::move(onConnectionLost)),
      connectionDrainingHandler(std::move(onConnectionDraining)),
      pingIntervalSec(config.pingIntervalSec),
      pingTimeoutSec(config.pingTimeoutSec),
      session(new Http2Session(ep, config)) {}

bool Connection::connect() {
  CHTTP2_LOG_INFO("conn %zu: connecting", connId);

  if (!session->start()) {
    CHTTP2_LOG_ERROR("conn %zu: failed to start session", connId);
    return false;
  }

  sessionFd = session->pollFd();
  if (sessionFd == INVALID_SOCKET) {
    CHTTP2_LOG_ERROR("conn %zu: invalid session fd", connId);
    session->close();
    return false;
  }

  connected = true;
  CHTTP2_LOG_INFO("conn %zu: connected, sessionFd=%d", connId, fdToInt(sessionFd));
  return true;
}

bool Connection::registerWithReactor() {
  if (!connected || sessionFd == INVALID_SOCKET) {
    return false;
  }

  bool registered = reactor.registerFd(
      sessionFd, [this] { onSessionReadable(); }, [this] { onSessionWritable(); });
  if (!registered) {
    CHTTP2_LOG_ERROR("conn %zu: failed to register session fd=%d", connId, fdToInt(sessionFd));
    connected = false;
    session->close();
    return false;
  }

  reactor.enableWrite(sessionFd, session->wantsWrite());
  schedulePingTimer();
  return true;
}

void Connection::closeOnReactor() {
  CHTTP2_ASSERT_REACTOR_THREAD();
  if (!connected) {
    return;
  }
  connected = false;
  cancelPingTimers();

  if (sessionFd != INVALID_SOCKET) {
    reactor.unregisterFd(sessionFd);
    sessionFd = INVALID_SOCKET;
  }

  if (session) {
    auto events = session->close();
    processSessionEvents(events);
  }
}

bool Connection::submitOnReactor(const chttp2::Request& request, const RequestContextPtr& context) {
  CHTTP2_ASSERT_REACTOR_THREAD();
  if (!connected || !session) {
    return false;
  }

  auto events = session->submit(request, context);
  processSessionEvents(events);
  updateWriteInterest();
  return true;
}

void Connection::cancelRequestOnReactor(const RequestContextPtr& context) {
  CHTTP2_ASSERT_REACTOR_THREAD();
  if (!session) {
    return;
  }

  auto events = session->cancelRequest(context);
  processSessionEvents(events);
  updateWriteInterest();
}

bool Connection::canAcceptNewRequests() const {
  return connected && session && session->isHealthy() && !session->isGoingAway();
}

bool Connection::hasStreamCapacity() const {
  return session && session->hasStreamCapacity();
}

std::size_t Connection::activeStreamCount() const {
  return session ? session->activeStreamCount() : 0;
}

void Connection::onSessionReadable() {
  CHTTP2_ASSERT_REACTOR_THREAD();
  if (!session || !connected) {
    return;
  }
  auto events = session->onReadable();
  processSessionEvents(events);
  updateWriteInterest();
}

void Connection::onSessionWritable() {
  CHTTP2_ASSERT_REACTOR_THREAD();
  if (!session || !connected) {
    return;
  }
  session->onWritable();
  updateWriteInterest();
}

void Connection::updateWriteInterest() {
  if (!session || sessionFd == INVALID_SOCKET) {
    return;
  }
  reactor.enableWrite(sessionFd, session->wantsWrite());
}

void Connection::processSessionEvents(const std::vector<SessionEvent>& events) {
  for (const auto& event : events) {
    switch (event.type) {
      case SessionEvent::Type::REQUEST_DONE:
        requestDoneHandler(event.context, event.response);
        break;
      case SessionEvent::Type::SESSION_CLOSING:
        if (connectionDrainingHandler) {
          connectionDrainingHandler(this);
        }
        break;
      case SessionEvent::Type::SESSION_DEAD:
        connected = false;
        cancelPingTimers();
        if (sessionFd != INVALID_SOCKET) {
          reactor.unregisterFd(sessionFd);
          sessionFd = INVALID_SOCKET;
        }
        if (connectionLostHandler) {
          connectionLostHandler(this);
        }
        break;
      case SessionEvent::Type::PING_ACK:
        if (pendingPing) {
          pendingPing = false;
          if (pingTimeoutTimerId != 0) {
            reactor.cancelTimer(pingTimeoutTimerId);
            pingTimeoutTimerId = 0;
          }
          schedulePingTimer();
        }
        break;
    }
  }
}

void Connection::schedulePingTimer() {
  if (pingIntervalSec <= 0 || !connected) {
    return;
  }
  pingTimerId = reactor.scheduleOnce(pingIntervalSec * 1000, [this]() { onPingTimer(); });
}

void Connection::onPingTimer() {
  pingTimerId = 0;
  if (!connected || !session) {
    return;
  }

  // Skip if there are active streams — data flow is the heartbeat.
  if (session->activeStreamCount() > 0) {
    schedulePingTimer();
    return;
  }

  // Skip if a ping is already outstanding.
  if (pendingPing) {
    return;
  }

  CHTTP2_LOG_DEBUG("conn %zu: sending keepalive PING", connId);
  auto events = session->sendPing();
  processSessionEvents(events);
  updateWriteInterest();

  pendingPing = true;
  if (pingTimeoutSec > 0) {
    pingTimeoutTimerId = reactor.scheduleOnce(pingTimeoutSec * 1000, [this]() { onPingTimeout(); });
  }
}

void Connection::onPingTimeout() {
  pingTimeoutTimerId = 0;
  if (!pendingPing || !connected) {
    return;
  }

  CHTTP2_LOG_WARN("conn %zu: PING timeout, marking connection dead", connId);
  pendingPing = false;
  closeOnReactor();
  if (connectionLostHandler) {
    connectionLostHandler(this);
  }
}

void Connection::cancelPingTimers() {
  if (pingTimerId != 0) {
    reactor.cancelTimer(pingTimerId);
    pingTimerId = 0;
  }
  if (pingTimeoutTimerId != 0) {
    reactor.cancelTimer(pingTimeoutTimerId);
    pingTimeoutTimerId = 0;
  }
  pendingPing = false;
}

}  // namespace chttp2
