// Client configuration for production use.
// Shows keepalive, PING heartbeat, and HTTP/2 SETTINGS tuning.
//
// Build:
//   cmake -Bbuild -Dchttp2_BUILD_EXAMPLES=ON && cmake --build build
// Run:
//   ./build/bin/client_config

#include <cstdio>

#include "chttp2/client.hpp"

int main() {
  chttp2::ClientConfig config;

  // TCP keepalive — detect dead connections at the OS level.
  // Internally derives: idle = 30s, interval = 5s, count = 3.
  config.keepAliveSec = 30;

  // HTTP/2 PING heartbeat — application-level liveness check.
  // Sends a PING frame every 15s when idle; closes if no response in 5s.
  config.pingIntervalSec = 15;
  config.pingTimeoutSec = 5;

  // HTTP/2 SETTINGS sent to the peer.
  config.maxConcurrentStreams = 200;   // allow more parallel streams
  config.initialWindowSize = 1 << 20; // 1 MB flow-control window
  config.maxFrameSize = 1 << 14;      // 16 KB (default)
  config.headerTableSize = 4096;      // HPACK table size

  chttp2::Client client(config);

  chttp2::Endpoint ep;
  ep.host = "nghttp2.org";
  ep.port = 443;
  ep.useSSL = true;

  auto resp = client.Get(ep, "/");

  if (resp.isError) {
    fprintf(stderr, "Error: %s\n", resp.errorMsg.c_str());
    return 1;
  }

  printf("Status: %d\n", resp.getStatusCode());
  printf("Server: %s\n", resp.getHeader("server").c_str());
  printf("Body: %zu bytes\n", resp.body.size());
  return 0;
}
