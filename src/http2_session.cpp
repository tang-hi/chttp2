#include "chttp2/http2_session.hpp"

#include <cstdint>
#include <iterator>

#include "chttp2/http2_constants.hpp"
#include "chttp2/log.hpp"

namespace chttp2 {

bool Http2Session::start() {
  if (started) {
    return true;
  }

  try {
    transport = StreamTransportFactory::create(endpoint, config.connectTimeoutMs);
  } catch (const std::exception& e) {
    CHTTP2_LOG_ERROR("session: transport creation failed: %s", e.what());
    return false;
  } catch (...) {
    CHTTP2_LOG_ERROR("session: transport creation failed: unknown exception");
    return false;
  }

  if (!transport || !transport->init(config.keepAliveSec)) {
    CHTTP2_LOG_ERROR("session: transport init failed");
    return false;
  }

  ProtocolStepResult result = protocol.start();
  std::vector<ProtocolEvent> discarded;
  consumeStepResult(std::move(result), &discarded);

  started = true;
  healthy = true;
  CHTTP2_LOG_INFO("session: started, fd=%d", pollFd());
  return true;
}

std::vector<SessionEvent> Http2Session::close() {
  if (!started) {
    return {};
  }
  CHTTP2_LOG_INFO("session: closing");

  started = false;
  healthy = false;
  goingAway = false;

  ProtocolStepResult result = protocol.close();
  std::vector<ProtocolEvent> discarded;
  consumeStepResult(std::move(result), &discarded);
  outbound.clear();

  if (transport) {
    transport->close();
    transport.reset();
  }

  return failAllStreams(Response::failConnectionError("Connection closed"));
}

bool Http2Session::isHealthy() const {
  return started && healthy && !protocol.isClosed();
}

bool Http2Session::isGoingAway() const {
  return goingAway;
}

bool Http2Session::hasStreamCapacity() const {
  return protocol.activeStreamCount() < protocol.peerMaxConcurrentStreams();
}

std::size_t Http2Session::activeStreamCount() const {
  return protocol.activeStreamCount();
}

std::vector<SessionEvent> Http2Session::submit(const chttp2::Request& request,
                                               const RequestContextPtr& context) {
  if (!isHealthy()) {
    std::vector<SessionEvent> events;
    SessionEvent ev;
    ev.type = SessionEvent::Type::REQUEST_DONE;
    ev.context = context;
    ev.response = Response::protocolError(ErrorCode::REFUSED_STREAM, "Session not healthy");
    events.push_back(std::move(ev));
    return events;
  }

  ProtocolStepResult result = protocol.submitRequest(request);
  StreamId streamId = 0;
  if (result.hasSubmittedStream) {
    streamId = result.submittedStreamId;
    context->setStreamId(streamId);
    streamContexts[streamId] = context;
    CHTTP2_LOG_DEBUG("session: submit req=%zu -> stream=%u", context->requestId(), streamId);
  }

  std::vector<ProtocolEvent> protocolEvents;
  consumeStepResult(std::move(result), &protocolEvents);
  auto events = translateEvents(std::move(protocolEvents));

  if (streamId != 0 && context->isCanceled() && !context->isFinished()) {
    ProtocolStepResult cancelResult = protocol.cancelStream(streamId, ErrorCode::CANCEL);
    std::vector<ProtocolEvent> cancelProtocolEvents;
    consumeStepResult(std::move(cancelResult), &cancelProtocolEvents);
    auto cancelEvents = translateEvents(std::move(cancelProtocolEvents));
    events.insert(events.end(), cancelEvents.begin(), cancelEvents.end());

    auto it = streamContexts.find(streamId);
    if (it != streamContexts.end()) {
      SessionEvent ev;
      ev.type = SessionEvent::Type::REQUEST_DONE;
      ev.context = std::move(it->second);
      ev.response = Response::customError("Request canceled");
      events.push_back(std::move(ev));
      streamContexts.erase(it);
    }
  }

  return events;
}

std::vector<SessionEvent> Http2Session::cancelRequest(const RequestContextPtr& context) {
  if (!context) {
    return {};
  }

  StreamId streamId = context->getStreamId();
  if (streamId == 0 || !isHealthy()) {
    return {};
  }

  auto it = streamContexts.find(streamId);
  if (it == streamContexts.end()) {
    return {};
  }

  CHTTP2_LOG_DEBUG("session: cancel stream=%u", streamId);
  ProtocolStepResult result = protocol.cancelStream(streamId, ErrorCode::CANCEL);
  std::vector<ProtocolEvent> protocolEvents;
  consumeStepResult(std::move(result), &protocolEvents);
  auto events = translateEvents(std::move(protocolEvents));

  auto it2 = streamContexts.find(streamId);
  if (it2 != streamContexts.end()) {
    SessionEvent ev;
    ev.type = SessionEvent::Type::REQUEST_DONE;
    ev.context = std::move(it2->second);
    ev.response = Response::customError("Request canceled");
    events.push_back(std::move(ev));
    streamContexts.erase(it2);
  }

  return events;
}

std::vector<SessionEvent> Http2Session::sendPing() {
  if (!isHealthy()) {
    return {};
  }
  ProtocolStepResult result = protocol.sendPing();
  for (auto& buf : result.outbound) {
    enqueueOutbound(std::move(buf));
  }
  return {};
}

int Http2Session::pollFd() const {
  if (!transport) {
    return -1;
  }
  return transport->fd();
}

bool Http2Session::wantsWrite() const {
  return !outbound.empty();
}

std::vector<SessionEvent> Http2Session::onReadable() {
  if (!isHealthy()) {
    return {};
  }

  std::vector<SessionEvent> sessionEvents;
  std::uint8_t buffer[8192];
  while (true) {
    IOResult io = transport->readSome(buffer, sizeof(buffer));
    if (io.state == IOState::WOULD_BLOCK) {
      break;
    }
    if (io.state == IOState::CLOSED) {
      CHTTP2_LOG_WARN("session: peer closed connection");
      return handleTransportFailure();
    }
    if (io.state == IOState::ERROR) {
      CHTTP2_LOG_ERROR("session: read error");
      return handleTransportFailure();
    }
    if (io.bytes == 0) {
      break;
    }

    ProtocolStepResult result = protocol.receiveBytes(buffer, io.bytes);
    for (auto& slice : result.outbound) {
      enqueueOutbound(std::move(slice));
    }
    auto events = translateEvents(std::move(result.events));
    sessionEvents.insert(sessionEvents.end(),
                         std::make_move_iterator(events.begin()),
                         std::make_move_iterator(events.end()));
  }
  return sessionEvents;
}

void Http2Session::onWritable() {
  if (!isHealthy()) {
    return;
  }

  while (!outbound.empty()) {
    OutboundChunk& chunk = outbound.front();
    const std::uint8_t* begin = chunk.data.begin();
    std::size_t total = chunk.data.size();
    if (chunk.offset >= total) {
      outbound.pop_front();
      continue;
    }

    IOResult io = transport->writeSome(begin + chunk.offset, total - chunk.offset);
    if (io.state == IOState::WOULD_BLOCK) {
      return;
    }
    if (io.state == IOState::CLOSED || io.state == IOState::ERROR) {
      CHTTP2_LOG_WARN("session: write failure");
      handleTransportFailure();
      return;
    }

    chunk.offset += io.bytes;
    if (chunk.offset >= total) {
      outbound.pop_front();
    }
  }
}

void Http2Session::consumeStepResult(ProtocolStepResult&& result,
                                     std::vector<ProtocolEvent>* protocolEvents) {
  for (auto& buf : result.outbound) {
    enqueueOutbound(std::move(buf));
  }
  if (protocolEvents) {
    *protocolEvents = std::move(result.events);
  }
}

std::vector<SessionEvent> Http2Session::handleTransportFailure() {
  if (!healthy) {
    return {};
  }
  healthy = false;
  ProtocolStepResult result = protocol.onTransportClosed();
  std::vector<ProtocolEvent> discarded;
  consumeStepResult(std::move(result), &discarded);
  outbound.clear();
  if (transport) {
    transport->close();
  }

  auto events = failAllStreams(Response::failConnectionError("Connection lost"));
  SessionEvent dead;
  dead.type = SessionEvent::Type::SESSION_DEAD;
  events.push_back(std::move(dead));
  return events;
}

void Http2Session::enqueueOutbound(ByteBuffer&& data) {
  OutboundChunk chunk;
  chunk.data = std::move(data);
  chunk.offset = 0;
  outbound.push_back(std::move(chunk));
}

std::vector<SessionEvent> Http2Session::translateEvents(
    std::vector<ProtocolEvent>&& protocolEvents) {
  std::vector<SessionEvent> sessionEvents;

  for (auto& pe : protocolEvents) {
    switch (pe.type) {
      case ProtocolEventType::RESPONSE_COMPLETED: {
        auto it = streamContexts.find(pe.streamId);
        if (it != streamContexts.end()) {
          SessionEvent ev;
          ev.type = SessionEvent::Type::REQUEST_DONE;
          ev.context = std::move(it->second);
          ev.response = std::move(pe.response);
          sessionEvents.push_back(std::move(ev));
          streamContexts.erase(it);
        }
        break;
      }
      case ProtocolEventType::RESPONSE_FAILED: {
        auto it = streamContexts.find(pe.streamId);
        if (it != streamContexts.end()) {
          SessionEvent ev;
          ev.type = SessionEvent::Type::REQUEST_DONE;
          ev.context = std::move(it->second);
          ev.response = Response::protocolError(
              pe.errorCode, "errorCode=" + std::to_string(static_cast<uint32_t>(pe.errorCode)));
          sessionEvents.push_back(std::move(ev));
          streamContexts.erase(it);
        }
        break;
      }
      case ProtocolEventType::GOAWAY_RECEIVED:
        CHTTP2_LOG_WARN("session: received GOAWAY last_stream=%u error=%u debug=\"%s\"",
                        pe.lastStreamId,
                        static_cast<uint32_t>(pe.errorCode),
                        pe.debugData.c_str());
        goingAway = true;
        {
          SessionEvent ev;
          ev.type = SessionEvent::Type::SESSION_CLOSING;
          sessionEvents.push_back(std::move(ev));
        }
        break;
      case ProtocolEventType::PING_ACK_RECEIVED: {
        SessionEvent ev;
        ev.type = SessionEvent::Type::PING_ACK;
        sessionEvents.push_back(std::move(ev));
        break;
      }
      case ProtocolEventType::CONNECTION_ERROR:
      case ProtocolEventType::CONNECTION_CLOSED:
      case ProtocolEventType::TRANSPORT_CLOSED: {
        healthy = false;
        auto failEvents = failAllStreams(Response::failConnectionError("Connection lost"));
        sessionEvents.insert(sessionEvents.end(), failEvents.begin(), failEvents.end());
        SessionEvent ev;
        ev.type = SessionEvent::Type::SESSION_DEAD;
        sessionEvents.push_back(std::move(ev));
        break;
      }
      default:
        break;
    }
  }

  return sessionEvents;
}

std::vector<SessionEvent> Http2Session::failAllStreams(const Response& response) {
  std::vector<SessionEvent> events;
  if (streamContexts.empty()) {
    return events;
  }
  CHTTP2_LOG_WARN("session: failing %zu active streams", streamContexts.size());
  for (auto& kv : streamContexts) {
    SessionEvent ev;
    ev.type = SessionEvent::Type::REQUEST_DONE;
    ev.context = std::move(kv.second);
    ev.response = response;
    events.push_back(std::move(ev));
  }
  streamContexts.clear();
  return events;
}

}  // namespace chttp2
