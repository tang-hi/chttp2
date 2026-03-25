# chttp2

A lightweight HTTP/2 client library for C++11.

Framing, HPACK header compression, stream multiplexing, and flow control are handled internally — you deal with requests and responses.

## Features

- Synchronous and asynchronous (callback) request APIs
- Automatic connection pooling and multiplexing across multiple origins
- TLS with OpenSSL; plain-text h2c also supported
- TCP and Unix domain socket transports
- Configurable keepalive, PING heartbeat, and HTTP/2 SETTINGS
- No external dependencies beyond OpenSSL

## Requirements

- Linux or macOS (Windows is not supported)
- C++11 compiler (GCC 4.8+, Clang 3.4+)
- CMake 3.15+
- OpenSSL 1.1.0+

## Integration

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(chttp2
  GIT_REPOSITORY https://github.com/tang-hi/chttp2.git
  GIT_TAG main
)
FetchContent_MakeAvailable(chttp2)

target_link_libraries(your_target PRIVATE chttp2::chttp2)
```

### Single Header

Copy `single_include/chttp2.hpp` into your project. This is a self-contained amalgamation of all headers and sources.

In exactly one `.cpp` file, define `CHTTP2_IMPLEMENTATION` before including the header. This tells the compiler to compile the implementation in that translation unit. All other files include the header normally.

```cpp
// one_file.cpp
#define CHTTP2_IMPLEMENTATION
#include "chttp2.hpp"

// everywhere_else.cpp
#include "chttp2.hpp"
```

Compile with `-lssl -lcrypto -lpthread`.

To regenerate the amalgamation from source:

```bash
make amalgamate
```

## Quick Start

```cpp
#include "chttp2/client.hpp"

int main() {
    chttp2::Endpoint ep;
    ep.host = "example.com";
    ep.port = 443;
    ep.useSSL = true;

    chttp2::Client client;
    auto resp = client.Get(ep, "/");

    if (!resp.isError) {
        printf("%d\n", resp.getStatusCode());
        printf("%s\n", resp.body.c_str());
    }

    return 0;
}
```

## Usage

### POST with Headers

```cpp
chttp2::Client::Headers headers = {{"content-type", "application/json"}};
std::string body = R"({"key": "value"})";

auto resp = client.Post(ep, "/api/data", body, headers);
```

### Async Requests

```cpp
auto op = client.Get(ep, "/", [](const chttp2::Response& resp) {
    // called on completion
});

// cancel if needed
op.cancel();
```

### Client Configuration

```cpp
chttp2::ClientConfig config;
config.keepAliveSec = 60;         // TCP keepalive interval (0 = off)
config.pingIntervalSec = 30;      // HTTP/2 PING heartbeat (0 = off)
config.pingTimeoutSec = 10;       // PING timeout before closing
config.maxConcurrentStreams = 100; // per-connection stream limit

chttp2::Client client(config);
```

### Endpoint Options

```cpp
chttp2::Endpoint ep;
ep.host = "example.com";      // hostname (DNS resolved)
ep.port = 443;
ep.useSSL = true;
ep.certFile = "/path/to/ca";  // custom CA certificate
ep.ip = "93.184.216.34";      // direct IP, skips DNS
ep.sockFile = "/tmp/h2.sock"; // Unix domain socket
ep.type = chttp2::SocketType::UNIX;
```

For more detailed usage (POST with JSON, async requests, etc.), see the [examples](examples/) directory.

## Build

```bash
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## CLI Tool

A minimal command-line HTTP/2 client is included:

```bash
make binary
./build/bin/chttp2_get https://google.com
./build/bin/chttp2_get -v https://nghttp2.org   # verbose
```

## Testing

```bash
make test              # unit tests
make e2e               # end-to-end tests (requires Go)
make chaos             # chaos stress test
make bench             # performance benchmark
make amalgamate-test   # verify single-header amalgamation
```

## Contributing

Contributions are welcome. Feel free to open issues for bug reports, feature requests, or questions. Pull requests are appreciated — please keep changes focused and include relevant tests.

## Standards

This library implements the following RFCs:

- [RFC 9113](https://www.rfc-editor.org/rfc/rfc9113) — HTTP/2
- [RFC 7541](https://www.rfc-editor.org/rfc/rfc7541) — HPACK: Header Compression for HTTP/2

## License

MIT
