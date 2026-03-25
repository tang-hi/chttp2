// POST request with JSON body.
//
// Build:
//   cmake -Bbuild -Dchttp2_BUILD_EXAMPLES=ON && cmake --build build
// Run:
//   ./build/bin/post_json

#include <cstdio>

#include "chttp2/client.hpp"

int main() {
  chttp2::Endpoint ep;
  ep.host = "httpbin.org";
  ep.port = 443;
  ep.useSSL = true;

  chttp2::Client client;

  std::string body = R"({"name": "chttp2", "version": "0.0.1"})";
  chttp2::Client::Headers headers = {
      {"content-type", "application/json"},
      {"accept", "application/json"},
  };

  auto resp = client.Post(ep, "/post", body, headers);

  if (resp.isError) {
    fprintf(stderr, "Error: %s\n", resp.errorMsg.c_str());
    return 1;
  }

  printf("Status: %d\n", resp.getStatusCode());
  printf("Response:\n%s\n", resp.body.c_str());
  return 0;
}
