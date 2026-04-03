// Simple CLI tool: HTTP/2 GET a URL and print the response.
//
// Usage:
//   chttp2_get https://example.com
//   chttp2_get -v https://example.com      (verbose: show library logs)
//   chttp2_get https://example.com/path
//   chttp2_get http://localhost:8080/api

#include <cstdio>
#include <cstring>
#include <string>

#include "chttp2/client.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/log.hpp"
#include "chttp2/response.hpp"

namespace {

struct ParsedURL {
  std::string host;
  uint16_t port{0};
  bool ssl{false};
  std::string path;
};

bool parseURL(const char* url, ParsedURL* out) {
  std::string s(url);

  // Scheme.
  if (s.find("https://") == 0) {
    out->ssl = true;
    s = s.substr(8);
  } else if (s.find("http://") == 0) {
    out->ssl = false;
    s = s.substr(7);
  } else {
    std::fprintf(stderr, "error: URL must start with http:// or https://\n");
    return false;
  }

  // Split host[:port] and path.
  auto slashPos = s.find('/');
  std::string hostPort;
  if (slashPos != std::string::npos) {
    hostPort = s.substr(0, slashPos);
    out->path = s.substr(slashPos);
  } else {
    hostPort = s;
    out->path = "/";
  }

  // Split host and port.
  auto colonPos = hostPort.rfind(':');
  if (colonPos != std::string::npos) {
    out->host = hostPort.substr(0, colonPos);
    out->port = static_cast<uint16_t>(std::stoi(hostPort.substr(colonPos + 1)));
  } else {
    out->host = hostPort;
    out->port = out->ssl ? 443 : 80;
  }

  if (out->host.empty()) {
    std::fprintf(stderr, "error: empty host\n");
    return false;
  }

  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
    std::printf("Usage: chttp2_get [-v] <url>\n");
    std::printf("  e.g. chttp2_get https://www.example.com\n");
    std::printf("       chttp2_get -v https://nghttp2.org\n");
    return argc < 2 ? 1 : 0;
  }

  bool verbose = false;
  const char* url = nullptr;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "-v") == 0) {
      verbose = true;
    } else {
      url = argv[i];
    }
  }

  if (!url) {
    std::fprintf(stderr, "error: no URL provided\n");
    return 1;
  }

  if (verbose) {
    chttp2::setLogHandler([](chttp2::LogLevel level, const char* msg) {
      const char* prefix = "?";
      switch (level) {
        case chttp2::LogLevel::DEBUG: prefix = "DBG"; break;
        case chttp2::LogLevel::INFO:  prefix = "INF"; break;
        case chttp2::LogLevel::WARN:  prefix = "WRN"; break;
        case chttp2::LogLevel::ERR: prefix = "ERR"; break;
      }
      std::fprintf(stderr, "[%s] %s\n", prefix, msg);
    });
  }

  ParsedURL parsed;
  if (!parseURL(url, &parsed)) {
    return 1;
  }

  chttp2::Endpoint ep;
  ep.host = parsed.host;
  ep.port = parsed.port;
  ep.useSSL = parsed.ssl;

  std::fprintf(stderr, "> GET %s %s://%s:%d%s\n",
               parsed.path.c_str(),
               parsed.ssl ? "https" : "http",
               parsed.host.c_str(),
               parsed.port,
               parsed.path.c_str());

  chttp2::Client client;
  auto resp = client.Get(ep, parsed.path);

  if (resp.isError) {
    std::fprintf(stderr, "error: %s", resp.errorMsg.c_str());
    if (!resp.extraMsg.empty()) {
      std::fprintf(stderr, " (%s)", resp.extraMsg.c_str());
    }
    std::fprintf(stderr, "\n");
    client.close();
    return 1;
  }

  // Print status and headers to stderr.
  std::fprintf(stderr, "< %d\n", resp.getStatusCode());
  for (const auto& h : resp.headers) {
    if (h.name[0] == ':') {
      continue;  // Skip pseudo-headers.
    }
    std::fprintf(stderr, "< %s: %s\n", h.name.c_str(), h.value.c_str());
  }
  std::fprintf(stderr, "< \n");

  // Print body to stdout (pipeable).
  if (!resp.body.empty()) {
    std::fwrite(resp.body.data(), 1, resp.body.size(), stdout);
  }

  client.close();
  return 0;
}
