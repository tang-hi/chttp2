#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "chttp2/client.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/response.hpp"
#include "e2e_fixture.hpp"

// =============================================================================
// Group 1: Basic Request Lifecycle
// =============================================================================

TEST_CASE("E2E BasicRequestLifecycle", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  // GET /echo
  auto resp1 = client.Get(ep, "/echo");
  REQUIRE_FALSE(resp1.isError);
  REQUIRE(resp1.getStatusCode() == 200);
  REQUIRE(resp1.getHeader("content-type") == "application/json");
  REQUIRE(resp1.body.find(":method") != std::string::npos);

  // POST /echo-body
  auto resp2 = client.Post(ep, "/echo-body", "hello world");
  REQUIRE_FALSE(resp2.isError);
  REQUIRE(resp2.getStatusCode() == 200);
  REQUIRE(resp2.body == "hello world");

  // HEAD /head
  chttp2::Client::Request headReq;
  headReq.endpoint = ep;
  headReq.method = "HEAD";
  headReq.target = "/head";
  auto resp3 = client.Send(headReq);
  REQUIRE_FALSE(resp3.isError);
  REQUIRE(resp3.getStatusCode() == 200);
  REQUIRE(resp3.body.empty());
  REQUIRE_FALSE(resp3.getHeader("content-length").empty());

  client.close();
  server.stop();
}

TEST_CASE("E2E LargePayloads", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  SECTION("Large response: 1MB download") {
    auto resp = client.Get(ep, "/big?size=1048576");
    REQUIRE_FALSE(resp.isError);
    REQUIRE(resp.getStatusCode() == 200);
    REQUIRE(resp.body.size() == 1048576);
  }

  SECTION("Large request: 1MB upload") {
    std::string body(1048576, 'A');
    auto resp = client.Post(ep, "/upload", body);
    REQUIRE_FALSE(resp.isError);
    REQUIRE(resp.getStatusCode() == 200);
    REQUIRE(resp.body.find("1048576") != std::string::npos);
  }

  client.close();
  server.stop();
}

TEST_CASE("E2E SpecialResponses", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  SECTION("204 No Content") {
    auto resp = client.Get(ep, "/status/204");
    REQUIRE_FALSE(resp.isError);
    REQUIRE(resp.getStatusCode() == 204);
    REQUIRE(resp.body.empty());
  }

  SECTION("Trailers") {
    auto resp = client.Get(ep, "/trailers");
    REQUIRE_FALSE(resp.isError);
    REQUIRE(resp.getStatusCode() == 200);
    REQUIRE(resp.body == "response with trailers");
    // Verify trailers were received.
    bool hasChecksum = false;
    for (const auto& t : resp.trailers) {
      if (t.name == "x-checksum" && t.value == "abc123") {
        hasChecksum = true;
      }
    }
    REQUIRE(hasChecksum);
  }

  SECTION("Large headers triggering CONTINUATION") {
    auto resp = client.Get(ep, "/headers/large");
    REQUIRE_FALSE(resp.isError);
    REQUIRE(resp.getStatusCode() == 200);
    // Verify at least some of the large headers are present.
    REQUIRE_FALSE(resp.getHeader("x-large-header-000").empty());
    REQUIRE_FALSE(resp.getHeader("x-large-header-050").empty());
    REQUIRE_FALSE(resp.getHeader("x-large-header-099").empty());
  }

  client.close();
  server.stop();
}

// =============================================================================
// Group 2: Concurrency and Connection Reuse
// =============================================================================

TEST_CASE("E2E ConcurrentStreams", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  constexpr int kTotal = 50;
  std::vector<std::future<chttp2::Response>> futures;
  futures.reserve(kTotal);

  for (int i = 0; i < kTotal; i++) {
    futures.push_back(std::async(std::launch::async, [&client, &ep, i]() {
      std::string path = "/delay?ms=100&id=" + std::to_string(i);
      return client.Get(ep, path);
    }));
  }

  int successCount = 0;
  for (auto& f : futures) {
    auto resp = f.get();
    if (!resp.isError && resp.getStatusCode() == 200) {
      successCount++;
    }
  }
  REQUIRE(successCount == kTotal);

  client.close();
  server.stop();
}

TEST_CASE("E2E MultiOriginPool", "[e2e]") {
  GoServerProcess serverA;
  GoServerProcess serverB;
  REQUIRE(serverA.start());
  REQUIRE(serverB.start());
  auto epA = serverA.endpoint();
  auto epB = serverB.endpoint();

  chttp2::Client client;

  // Interleave requests across two origins.
  for (int i = 0; i < 5; i++) {
    auto rA = client.Get(epA, "/echo");
    REQUIRE_FALSE(rA.isError);
    auto rB = client.Get(epB, "/echo");
    REQUIRE_FALSE(rB.isError);
  }

  // Kill server A, verify B still works and A fails gracefully.
  serverA.stop();

  auto rB = client.Get(epB, "/echo");
  REQUIRE_FALSE(rB.isError);
  REQUIRE(rB.getStatusCode() == 200);

  auto rA = client.Get(epA, "/ping");
  REQUIRE(rA.isError);

  client.close();
  serverB.stop();
}

// =============================================================================
// Group 3: Timeout and Cancellation
// =============================================================================

TEST_CASE("E2E TimeoutAndCancel", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  SECTION("Request timeout") {
    chttp2::Client::Options opts;
    opts.timeoutMs = 200;

    auto start = std::chrono::steady_clock::now();
    auto resp = client.Get(ep, "/delay?ms=5000", {}, opts);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    REQUIRE(resp.isError);
    REQUIRE(elapsed.count() < 2000);
  }

  SECTION("Cancel in-flight request") {
    std::promise<chttp2::Response> promise;
    auto future = promise.get_future();

    auto op = client.Get(ep, "/delay?ms=5000", [&promise](const chttp2::Response& r) {
      try {
        promise.set_value(r);
      } catch (const std::future_error&) { // NOLINT(bugprone-empty-catch)
        // Duplicate completion is harmless.
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(op.cancel());

    auto resp = future.get();
    REQUIRE(resp.isError);
  }

  SECTION("No timeout on slow server") {
    chttp2::Client::Options opts;
    opts.timeoutMs = -1;

    auto resp = client.Get(ep, "/delay?ms=2000", {}, opts);
    REQUIRE_FALSE(resp.isError);
    REQUIRE(resp.getStatusCode() == 200);
  }

  client.close();
  server.stop();
}

// =============================================================================
// Group 4: Connection Failure and Recovery
// =============================================================================

TEST_CASE("E2E ServerAbruptClose", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  // Warm up connection.
  auto r1 = client.Get(ep, "/ping");
  REQUIRE_FALSE(r1.isError);
  REQUIRE(r1.getStatusCode() == 200);

  // Server abruptly closes TCP.
  auto r2 = client.Get(ep, "/close");
  REQUIRE(r2.isError);

  // Auto-reconnect on next request.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto r3 = client.Get(ep, "/ping");
  REQUIRE_FALSE(r3.isError);
  REQUIRE(r3.getStatusCode() == 200);

  client.close();
  server.stop();
}

TEST_CASE("E2E ServerGoaway", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  // Warm up connection.
  auto r1 = client.Get(ep, "/ping");
  REQUIRE_FALSE(r1.isError);

  // Server sends GOAWAY.
  auto r2 = client.Get(ep, "/goaway");
  // The request may succeed or fail depending on timing.
  // What matters is that the client recovers.
  (void)r2;

  // Wait for the connection to be cleaned up.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // New request should use a new connection and succeed.
  auto r3 = client.Get(ep, "/ping");
  REQUIRE_FALSE(r3.isError);
  REQUIRE(r3.getStatusCode() == 200);

  client.close();
  server.stop();
}

TEST_CASE("E2E ServerResetStream", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  // Server sends RST_STREAM on this request.
  auto r1 = client.Get(ep, "/reset");
  REQUIRE(r1.isError);

  // Connection should still be usable for new requests.
  auto r2 = client.Get(ep, "/ping");
  REQUIRE_FALSE(r2.isError);
  REQUIRE(r2.getStatusCode() == 200);

  client.close();
  server.stop();
}

// =============================================================================
// Group 5: Thread Safety
// =============================================================================

TEST_CASE("E2E ConcurrentSendFromMultipleThreads", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  constexpr int kThreads = 8;
  constexpr int kPerThread = 50;
  std::atomic<int> successCount{0};
  std::atomic<int> errorCount{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&client, &ep, &successCount, &errorCount]() {
      for (int i = 0; i < kPerThread; i++) {
        auto resp = client.Get(ep, "/ping");
        if (!resp.isError && resp.getStatusCode() == 200) {
          successCount.fetch_add(1);
        } else {
          errorCount.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  int total = successCount.load() + errorCount.load();
  REQUIRE(total == kThreads * kPerThread);
  REQUIRE(successCount.load() == kThreads * kPerThread);

  client.close();
  server.stop();
}

TEST_CASE("E2E SendAndCloseRace", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  std::atomic<bool> stopSending{false};
  std::atomic<int> callbackCount{0};

  // Thread A: send as fast as possible.
  std::thread sender([&]() {
    while (!stopSending.load()) {
      client.Get(ep, "/delay?ms=50", [&callbackCount](const chttp2::Response&) {
        callbackCount.fetch_add(1);
      });
    }
  });

  // Thread B: sleep then close.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stopSending.store(true);
  client.close();

  sender.join();

  // No crash, no hang. Every sent request got a callback.
  REQUIRE(callbackCount.load() > 0);

  server.stop();
}

// =============================================================================
// Group 6: Resource Cleanup
// =============================================================================

TEST_CASE("E2E CleanShutdown", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  auto countFds = []() -> int {
    int count = 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", getpid());
    if (auto* dir = opendir(path)) {
      while (readdir(dir)) {
        count++;
      }
      closedir(dir);
    }
    return count;
  };

  int fdsBefore = countFds();

  {
    chttp2::Client client;
    for (int i = 0; i < 10; i++) {
      auto resp = client.Get(ep, "/ping");
      REQUIRE_FALSE(resp.isError);
    }
    client.close();
  }

  // Small delay for async cleanup.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  int fdsAfter = countFds();
  // Allow some tolerance (timer fds, etc.), but no major leak.
  REQUIRE(fdsAfter <= fdsBefore + 5);

  server.stop();
}

TEST_CASE("E2E RepeatedConnectDisconnect", "[e2e]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  int totalSuccess = 0;

  for (int iter = 0; iter < 20; iter++) {
    chttp2::Client client;
    for (int i = 0; i < 10; i++) {
      auto resp = client.Get(ep, "/ping");
      if (!resp.isError && resp.getStatusCode() == 200) {
        totalSuccess++;
      }
    }
    client.close();
  }

  REQUIRE(totalSuccess == 200);

  server.stop();
}

// =============================================================================
// Group 7: TLS
// =============================================================================

TEST_CASE("E2E TlsBasicGet", "[e2e][tls]") {
  GoServerProcess server;
  REQUIRE(server.start(true));
  auto ep = server.tlsEndpoint();

  chttp2::Client client;
  auto resp = client.Get(ep, "/echo");
  REQUIRE_FALSE(resp.isError);
  REQUIRE(resp.getStatusCode() == 200);
  REQUIRE(resp.getHeader("content-type") == "application/json");

  client.close();
  server.stop();
}

TEST_CASE("E2E TlsBadCert", "[e2e][tls]") {
  GoServerProcess server;
  REQUIRE(server.start(true));

  // Connect without specifying the cert file (default CA won't trust self-signed).
  chttp2::Endpoint ep;
  ep.host = "127.0.0.1";
  ep.port = static_cast<uint16_t>(server.port());
  ep.useSSL = true;

  chttp2::Client client;
  auto resp = client.Get(ep, "/echo");
  REQUIRE(resp.isError);

  client.close();
  server.stop();
}

// =============================================================================
// Group 8: Stress
// =============================================================================

TEST_CASE("E2E HighThroughput", "[e2e][stress]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  constexpr int kTotal = 10000;
  std::atomic<int> successCount{0};
  std::atomic<int> doneCount{0};

  for (int i = 0; i < kTotal; i++) {
    client.Get(ep, "/ping", [&successCount, &doneCount](const chttp2::Response& r) {
      if (!r.isError && r.getStatusCode() == 200) {
        successCount.fetch_add(1);
      }
      doneCount.fetch_add(1);
    });
  }

  // Wait up to 60 seconds for all to complete.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (doneCount.load() < kTotal && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  REQUIRE(doneCount.load() == kTotal);
  REQUIRE(successCount.load() == kTotal);

  client.close();
  server.stop();
}

TEST_CASE("E2E SustainedLoad", "[e2e][stress]") {
  GoServerProcess server;
  REQUIRE(server.start());
  auto ep = server.endpoint();

  chttp2::Client client;

  constexpr int kDurationSec = 5;
  constexpr int kRate = 100;  // requests/sec
  std::atomic<int> successCount{0};
  std::atomic<int> doneCount{0};

  auto startTime = std::chrono::steady_clock::now();
  int totalSent = 0;

  for (int sec = 0; sec < kDurationSec; sec++) {
    for (int i = 0; i < kRate; i++) {
      client.Get(ep, "/ping", [&successCount, &doneCount](const chttp2::Response& r) {
        if (!r.isError && r.getStatusCode() == 200) {
          successCount.fetch_add(1);
        }
        doneCount.fetch_add(1);
      });
      totalSent++;
    }
    // Pace to ~100 rps.
    auto target = startTime + std::chrono::seconds(sec + 1);
    std::this_thread::sleep_until(target);
  }

  // Wait for all to complete (up to 30s).
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (doneCount.load() < totalSent && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  REQUIRE(doneCount.load() == totalSent);
  REQUIRE(successCount.load() == totalSent);

  client.close();
  server.stop();
}
