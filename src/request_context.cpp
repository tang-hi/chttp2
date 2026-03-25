#include "chttp2/request_context.hpp"

#include <utility>

namespace chttp2 {

RequestContext::RequestContext(uint64_t requestId, int timeoutMs, CompletionHandler completion)
    : REQUEST_ID(requestId), TIMEOUT_MS(timeoutMs), completionHandler(std::move(completion)) {}

bool RequestContext::tryFinish() {
  bool expected = false;
  return finished.compare_exchange_strong(expected, true);
}

bool RequestContext::markCanceled() {
  bool expected = false;
  return canceled.compare_exchange_strong(expected, true);
}

void RequestContext::setCancelHandler(const CancelHandler& handler) {
  std::lock_guard<std::mutex> lock(mutex);
  cancelHandler = handler;
}

bool RequestContext::cancel() {
  if (isFinished()) {
    return false;
  }

  if (!markCanceled()) {
    return false;
  }

  CancelHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex);
    handler = cancelHandler;
  }

  if (handler) {
    handler(REQUEST_ID);
  }
  return true;
}

void RequestContext::invokeCompletion(const Response& response) const {
  CompletionHandler callback;
  {
    std::lock_guard<std::mutex> lock(mutex);
    callback = completionHandler;
  }

  if (callback) {
    callback(response);
  }
}

}  // namespace chttp2
