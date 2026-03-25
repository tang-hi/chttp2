// Request timeout and cancellation.
//
// Build:
//   cmake -Bbuild -Dchttp2_BUILD_EXAMPLES=ON && cmake --build build
// Run:
//   ./build/bin/timeout_cancel

#include <chrono>
#include <cstdio>
#include <thread>

#include "chttp2/client.hpp"

int main() {
  chttp2::Endpoint ep;
  ep.host = "httpbin.org";
  ep.port = 443;
  ep.useSSL = true;

  chttp2::Client client;

  // --- Timeout ---
  // Set a 2-second timeout. If the server doesn't respond in time,
  // resp.isError will be true.
  chttp2::Client::Options opts;
  opts.timeoutMs = 2000;

  printf("GET /delay/1 with 2s timeout...\n");
  auto resp = client.Get(ep, "/delay/1", {}, opts);
  printf("  result: %s (status=%d)\n",
         resp.isError ? resp.errorMsg.c_str() : "ok",
         resp.getStatusCode());

  printf("GET /delay/10 with 2s timeout...\n");
  resp = client.Get(ep, "/delay/10", {}, opts);
  printf("  result: %s\n",
         resp.isError ? resp.errorMsg.c_str() : "ok");

  // --- Cancellation ---
  // Fire an async request, then cancel it before completion.
  printf("\nAsync GET /delay/10, cancelling after 100ms...\n");
  auto op = client.Get(ep, "/delay/10", [](const chttp2::Response& r) {
    printf("  callback: %s\n",
           r.isError ? r.errorMsg.c_str() : "ok");
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  bool cancelled = op.cancel();
  printf("  cancel() returned %s\n", cancelled ? "true" : "false");

  // Give the callback a moment to fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return 0;
}
