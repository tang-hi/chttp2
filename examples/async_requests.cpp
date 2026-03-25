// Concurrent async requests with callbacks.
// All requests are multiplexed over a single HTTP/2 connection.
//
// Build:
//   cmake -Bbuild -Dchttp2_BUILD_EXAMPLES=ON && cmake --build build
// Run:
//   ./build/bin/async_requests

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "chttp2/client.hpp"

int main() {
  chttp2::Endpoint ep;
  ep.host = "nghttp2.org";
  ep.port = 443;
  ep.useSSL = true;

  chttp2::Client client;

  const int total = 5;
  std::atomic<int> done(0);

  // Fire off multiple requests — they share one connection via multiplexing.
  for (int i = 0; i < total; ++i) {
    client.Get(ep, "/", [i, &done](const chttp2::Response& resp) {
      if (resp.isError) {
        printf("[%d] error: %s\n", i, resp.errorMsg.c_str());
      } else {
        printf("[%d] status=%d, %zu bytes\n", i, resp.getStatusCode(), resp.body.size());
      }
      ++done;
    });
  }

  // Wait for all callbacks.
  while (done < total) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  printf("All %d requests completed.\n", total);
  return 0;
}
