#include "chttp2/flow.hpp"

#include <cstdint>
#include <stdexcept>

namespace chttp2 {
int32_t InFlow::add(int32_t n) {
  int64_t sum = static_cast<int64_t>(unsnt) + n;
  // RFC 9113 Section 6.9.1: "A sender MUST NOT allow a flow-control window
  // to exceed 2^31-1 octets."  Return -1 to signal overflow so the caller
  // can emit FLOW_CONTROL_ERROR instead of crashing.
  if (sum > INT32_MAX) {
    return -1;
  }
  unsnt = static_cast<int32_t>(sum);

  if (unsnt < INFLOW_MIN_REFRESH && unsnt < avail) {
    return 0;
  }
  int32_t refresh = unsnt;
  avail += unsnt;
  unsnt = 0;
  return refresh;
}

bool InFlow::take(uint32_t n) {
  if (avail <= 0) {
    return false;
  }
  if (static_cast<uint32_t>(avail) < n) {
    return false;
  }
  avail -= static_cast<int32_t>(n);
  return true;
}

bool InFlow::takeInflows(InFlow* f1, InFlow* f2, uint32_t n) {
  // RFC 9113 §6.9: zero-length DATA frames do not consume flow control.
  if (n == 0) {
    return true;
  }
  if (f1->avail <= 0 || f2->avail <= 0) {
    return false;
  }
  if (static_cast<uint32_t>(f1->avail) < n || static_cast<uint32_t>(f2->avail) < n) {
    return false;
  }
  f1->avail -= static_cast<int32_t>(n);
  f2->avail -= static_cast<int32_t>(n);
  return true;
}

int32_t OutFlow::available() {
  auto n = window;
  if (conn != nullptr && conn->window < n) {
    n = conn->window;
  }
  return n;
}

void OutFlow::take(int32_t n) {
  if (n > available()) {
    throw std::runtime_error("internal error: took too much");
  }
  window -= n;
  if (conn != nullptr) {
    conn->window -= n;
  }
}

bool OutFlow::add(int32_t n) {
  // Use int64_t arithmetic to detect overflow without triggering UB.
  int64_t sum = static_cast<int64_t>(window) + n;
  if (sum > INT32_MAX || sum < INT32_MIN) {
    return false;
  }
  window = static_cast<int32_t>(sum);
  return true;
}

};  // namespace chttp2
