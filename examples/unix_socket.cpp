// Connect via Unix domain socket (h2c, no TLS).
// Common for local services: Docker, nginx, gRPC sidecars, etc.
//
// To test, start a local h2c server on a Unix socket, e.g.:
//   go run test/e2e/testserver/main.go -addr unix:///tmp/chttp2.sock
//
// Build:
//   cmake -Bbuild -Dchttp2_BUILD_EXAMPLES=ON && cmake --build build
// Run:
//   ./build/bin/unix_socket

#include <cstdio>

#include "chttp2/client.hpp"

int main() {
  chttp2::Endpoint ep;
  ep.type = chttp2::SocketType::UNIX;
  ep.sockFile = "/tmp/chttp2.sock";
  ep.host = "localhost";  // sent as :authority header
  ep.useSSL = false;      // h2c — plain-text HTTP/2

  chttp2::Client client;
  auto resp = client.Get(ep, "/");

  if (resp.isError) {
    fprintf(stderr, "Error: %s\n", resp.errorMsg.c_str());
    fprintf(stderr, "Make sure a h2c server is listening on %s\n", ep.sockFile.c_str());
    return 1;
  }

  printf("Status: %d\n", resp.getStatusCode());
  printf("Body:\n%s\n", resp.body.c_str());
  return 0;
}
