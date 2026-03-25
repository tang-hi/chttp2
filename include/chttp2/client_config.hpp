#pragma once

#include <cstdint>

namespace chttp2 {

struct ClientConfig {
  // TCP keepalive in seconds (0 = disabled).
  // Internally derived: idle = keepAliveSec, interval = keepAliveSec/6, count = 3.
  int keepAliveSec{60};

  // HTTP/2 PING heartbeat (0 = disabled).
  int pingIntervalSec{30};
  int pingTimeoutSec{10};

  // HTTP/2 SETTINGS (sent to peer in initial SETTINGS frame).
  std::uint32_t maxConcurrentStreams{100};
  std::uint32_t initialWindowSize{65535};
  std::uint32_t maxFrameSize{16384};
  std::uint32_t maxHeaderListSize{0};  // 0 = no limit
  std::uint32_t headerTableSize{4096};
};

}  // namespace chttp2
