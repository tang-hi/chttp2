#pragma once

#include <cstdint>
#include <vector>

#include "chttp2/request.hpp"
#include "chttp2/request_context.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

struct SessionEvent {
  enum class Type : std::uint8_t { REQUEST_DONE, SESSION_CLOSING, SESSION_DEAD, PING_ACK };

  Type type;
  RequestContextPtr context;
  Response response;
};

class IHttpSession {
 public:
  virtual ~IHttpSession() = default;

  virtual bool start() = 0;
  virtual std::vector<SessionEvent> close() = 0;
  virtual bool isHealthy() const = 0;
  virtual bool isGoingAway() const = 0;
  virtual bool hasStreamCapacity() const = 0;
  virtual std::size_t activeStreamCount() const = 0;

  // Session manages the internal stream/request mapping.
  // Returns events that may be generated during submit (e.g., immediate failure, cancel race).
  virtual std::vector<SessionEvent> submit(const chttp2::Request& request,
                                           const RequestContextPtr& context) = 0;
  virtual std::vector<SessionEvent> cancelRequest(const RequestContextPtr& context) = 0;

  virtual std::vector<SessionEvent> sendPing() = 0;

  virtual int pollFd() const = 0;
  virtual bool wantsWrite() const = 0;
  virtual std::vector<SessionEvent> onReadable() = 0;
  virtual void onWritable() = 0;
};

}  // namespace chttp2
