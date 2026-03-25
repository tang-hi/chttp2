# Examples

Build all examples:

```bash
cmake -Bbuild -Dchttp2_BUILD_EXAMPLES=ON
cmake --build build
```

| Example | What it shows |
|---------|---------------|
| `simple_get` | Basic HTTPS GET request |
| `post_json` | POST with JSON body and custom headers |
| `async_requests` | Concurrent async requests with callbacks, multiplexed on one connection |
| `timeout_cancel` | Request timeout (`Options::timeoutMs`) and `Operation::cancel()` |
| `client_config` | Production tuning: keepalive, PING heartbeat, HTTP/2 SETTINGS |
| `unix_socket` | Unix domain socket with plain-text h2c (Docker, nginx, sidecars) |

Run any example directly:

```bash
./build/bin/simple_get
./build/bin/post_json
./build/bin/async_requests
./build/bin/timeout_cancel
./build/bin/client_config
./build/bin/unix_socket    # needs a local h2c server on /tmp/chttp2.sock
```
