// Simple GET request over HTTPS.
//
// Build:
//   cmake -Bbuild -Dchttp2_BUILD_EXAMPLES=ON && cmake --build build
// Run:
//   ./build/bin/simple_get

#include <cstdio>

#include "chttp2/client.hpp"

int main() {
  chttp2::Endpoint ep;
  ep.host = "nghttp2.org";
  ep.port = 443;
  ep.useSSL = true;

  chttp2::Client client;
  auto resp = client.Get(ep, "/");

  if (resp.isError) {
    fprintf(stderr, "Error: %s\n", resp.errorMsg.c_str());
    return 1;
  }

  printf("Status: %d\n", resp.getStatusCode());
  printf("Content-Type: %s\n", resp.getHeader("content-type").c_str());
  printf("Body: %zu bytes\n", resp.body.size());
  return 0;
}
