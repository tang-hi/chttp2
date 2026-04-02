#pragma once

#include <deque>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chttp2/byte_buffer.hpp"
#include "chttp2/client_config.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/http2_protocol.hpp"
#include "chttp2/ihttp_session.hpp"
#include "chttp2/stream_transport.hpp"

namespace chttp2 {

class Http2Session : public IHttpSession {
 public:
  explicit Http2Session(Endpoint ep, const ClientConfig& cfg = ClientConfig())
      : endpoint(std::move(ep)), config(cfg), protocol(cfg) {}

  bool start() override;
  std::vector<SessionEvent> close() override;
  bool isHealthy() const override;
  bool isGoingAway() const override;
  bool hasStreamCapacity() const override;
  std::size_t activeStreamCount() const override;
  std::vector<SessionEvent> submit(const chttp2::Request& request,
                                   const RequestContextPtr& context) override;
  std::vector<SessionEvent> cancelRequest(const RequestContextPtr& context) override;

  std::vector<SessionEvent> sendPing() override;

  socket_t pollFd() const override;
  bool wantsWrite() const override;
  std::vector<SessionEvent> onReadable() override;
  void onWritable() override;

 private:
  struct OutboundChunk {
    ByteBuffer data;
    std::size_t offset = 0;
  };

  void consumeStepResult(ProtocolStepResult&& result, std::vector<ProtocolEvent>* protocolEvents);
  std::vector<SessionEvent> handleTransportFailure();
  void enqueueOutbound(ByteBuffer&& data);
  std::vector<SessionEvent> translateEvents(std::vector<ProtocolEvent>&& protocolEvents);
  std::vector<SessionEvent> failAllStreams(const Response& response);

  Endpoint endpoint;
  ClientConfig config;
  Http2Protocol protocol;
  std::unique_ptr<IStreamTransport> transport;
  std::deque<OutboundChunk> outbound;
  bool started = false;
  bool healthy = false;
  bool goingAway = false;

  std::unordered_map<StreamId, RequestContextPtr> streamContexts;
};

}  // namespace chttp2
