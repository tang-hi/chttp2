# Benchmarks

This directory contains performance benchmarks for the chttp2 library.

## Building Benchmarks

Benchmarks are not built by default. To build them, you need to enable the examples option:

```bash
mkdir build && cd build
cmake .. -Dchttp2_BUILD_EXAMPLES=1
cmake --build .
```

Note: The benchmarks currently use the same build flag as examples. In a future update, they may have their own flag.

## Available Benchmarks

### throughput.cpp

Measures request throughput (requests per second).

**Usage:**
```bash
./throughput <host> <port> <num_requests>
```

**Example:**
```bash
# Test against local server
./throughput localhost 8080 1000

# Test against HTTPS server
./throughput example.com 443 100
```

**Metrics:**
- Total time taken
- Average time per request
- Requests per second
- Success/failure count

## Creating a Test Server

For testing, you can use a simple Node.js HTTP/2 server:

```javascript
// server.js
const http2 = require('http2');

const server = http2.createServer();

server.on('stream', (stream, headers) => {
    stream.respond({
        'content-type': 'text/plain',
        ':status': 200
    });
    stream.end('Hello World!');
});

server.listen(8080);
console.log('HTTP/2 server running on http://localhost:8080');
```

Run with: `node server.js`

## Performance Considerations

### Connection Overhead

The first request includes connection establishment and HTTP/2 handshake. Subsequent requests reuse the connection, showing the benefit of connection multiplexing.

### SSL/TLS Impact

HTTPS connections have additional overhead from SSL handshake and encryption.

## Future Benchmarks

Planned benchmarks:

- Concurrent requests
- Large payloads
- Header compression efficiency
- Memory usage
- Latency distribution
