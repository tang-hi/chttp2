#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "chttp2/http2_constants.hpp"
#include "chttp2/response.hpp"

namespace chttp2 {

class RequestContext;
using RequestContextPtr = std::shared_ptr<RequestContext>;

class RequestContext {
 public:
  using CompletionHandler = std::function<void(const Response&)>;
  using CancelHandler = std::function<void(uint64_t)>;

  RequestContext(uint64_t requestId, int timeoutMs, CompletionHandler completion);

  uint64_t requestId() const { return REQUEST_ID; }

  int timeoutMs() const { return TIMEOUT_MS; }

  bool tryFinish();
  bool isFinished() const { return finished.load(); }

  bool markCanceled();
  bool isCanceled() const { return canceled.load(); }

  void setStreamId(StreamId sid) { streamId.store(sid); }

  StreamId getStreamId() const { return streamId.load(); }

  void setTimerId(uint64_t tid) { timerId.store(tid); }

  uint64_t getTimerId() const { return timerId.load(); }

  void setCancelHandler(const CancelHandler& handler);
  bool cancel();
  void invokeCompletion(const Response& response) const;

 private:
  const uint64_t REQUEST_ID;
  const int TIMEOUT_MS;
  CompletionHandler completionHandler;
  std::atomic<bool> finished{false};
  std::atomic<bool> canceled{false};
  std::atomic<StreamId> streamId{0};
  std::atomic<uint64_t> timerId{0};

  mutable std::mutex mutex;
  CancelHandler cancelHandler;
};

}  // namespace chttp2
