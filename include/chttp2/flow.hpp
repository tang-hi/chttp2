#pragma once
#include <cstdint>
namespace chttp2 {

// inflowMinRefresh is the minimum number of bytes we'll send for a flow control
// window update.
const int32_t INFLOW_MIN_REFRESH = 4 << 10;

struct InFlow {
  void init(int32_t n) { avail = n; }
  int32_t add(int32_t n);
  bool take(uint32_t n);
  static bool takeInflows(InFlow* f1, InFlow* f2, uint32_t n);
  int32_t available() const { return avail; }
  int32_t unsent() const { return unsnt; }

 private:
  int32_t avail{0};
  int32_t unsnt{0};
};

struct OutFlow {
  void setConnFlow(OutFlow* cf) { conn = cf; }

  int32_t available();

  void take(int32_t n);
  bool add(int32_t n);

 private:
  OutFlow* conn{nullptr};
  int32_t window{0};
};

};  // namespace chttp2
