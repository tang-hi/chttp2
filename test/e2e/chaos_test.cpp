// Chaos gate test — long-running, harsh conditions.
// Run with: make chaos
//
// Multi-phase stress test:
//   Phase 0: steady-state warm-up under full load
//   Phase 1: SIGKILL one server (crash)
//   Phase 2: GOAWAY another server (graceful drain)
//   Phase 3: TCP RST another server (abrupt close)
//   Phase 4: restart crashed servers, verify recovery
//   Phase 5: second wave of chaos (crash + GOAWAY simultaneously)
//   Phase 6: sustained load on surviving servers
//   Phase 7: final verification burst

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "chttp2/client.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/response.hpp"

#include "e2e_fixture.hpp"

namespace {

struct Counters {
  std::atomic<int> success{0};
  std::atomic<int> error{0};
  std::atomic<int> done{0};

  void record(const chttp2::Response& resp) {
    if (!resp.isError && resp.getStatusCode() >= 200 && resp.getStatusCode() < 300) {
      success.fetch_add(1, std::memory_order_relaxed);
    } else {
      error.fetch_add(1, std::memory_order_relaxed);
    }
    done.fetch_add(1, std::memory_order_relaxed);
  }

  void reset() {
    success.store(0);
    error.store(0);
    done.store(0);
  }

  void print(const char* label) const {
    std::printf("  %-12s  done=%d  success=%d  error=%d\n",
                label,
                done.load(),
                success.load(),
                error.load());
  }
};

// Mixed sync request workload: GET, POST, large download, delayed, upload.
void senderLoop(chttp2::Client& client,
                const chttp2::Endpoint& ep,
                Counters& ctr,
                std::atomic<bool>& stop) {
  chttp2::Client::Options opts;
  opts.timeoutMs = 5000;

  int i = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    chttp2::Response resp;
    switch (i % 6) {
      case 0:
        resp = client.Get(ep, "/ping", {}, opts);
        break;
      case 1:
        resp = client.Post(ep, "/echo-body", "chaos-payload-" + std::to_string(i), {}, opts);
        break;
      case 2:
        resp = client.Get(ep, "/big?size=65536", {}, opts);
        break;
      case 3:
        resp = client.Get(ep, "/delay?ms=20", {}, opts);
        break;
      case 4: {
        std::string body(4096, 'X');
        resp = client.Post(ep, "/upload", body, {}, opts);
        break;
      }
      case 5:
        resp = client.Get(ep, "/status/201", {}, opts);
        break;
      default:
        resp = client.Get(ep, "/ping", {}, opts);
        break;
    }
    ctr.record(resp);
    i++;
  }
}

// Async fire-and-forget workload (high throughput, callback-based).
void asyncBurstLoop(chttp2::Client& client,
                    const chttp2::Endpoint& ep,
                    Counters& ctr,
                    std::atomic<bool>& stop) {
  while (!stop.load(std::memory_order_relaxed)) {
    client.Get(ep, "/ping", [&ctr](const chttp2::Response& r) { ctr.record(r); });
    // Pace slightly to avoid overwhelming the submit queue.
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

void waitFor(std::chrono::milliseconds ms) {
  std::this_thread::sleep_for(ms);
}

using ms = std::chrono::milliseconds;

}  // namespace

TEST_CASE("Chaos gate test", "[chaos]") {
  // =========================================================================
  // Setup: 5 servers
  //   S0 = always stable (anchor)
  //   S1 = will be SIGKILL'd in phase 1, restarted in phase 4, killed again in phase 5
  //   S2 = will receive GOAWAY in phase 2, restarted in phase 4
  //   S3 = will get TCP RST in phase 3
  //   S4 = will receive GOAWAY in phase 5
  // =========================================================================

  std::printf("\n=== Chaos gate test ===\n\n");

  GoServerProcess s0;
  GoServerProcess s1;
  GoServerProcess s2;
  GoServerProcess s3;
  GoServerProcess s4;
  REQUIRE(s0.start());
  REQUIRE(s1.start());
  REQUIRE(s2.start());
  REQUIRE(s3.start());
  REQUIRE(s4.start());
  auto ep0 = s0.endpoint();
  auto ep1 = s1.endpoint();
  auto ep2 = s2.endpoint();
  auto ep3 = s3.endpoint();
  auto ep4 = s4.endpoint();

  chttp2::Client client;

  // Warm up all connections.
  REQUIRE_FALSE(client.Get(ep0, "/ping").isError);
  REQUIRE_FALSE(client.Get(ep1, "/ping").isError);
  REQUIRE_FALSE(client.Get(ep2, "/ping").isError);
  REQUIRE_FALSE(client.Get(ep3, "/ping").isError);
  REQUIRE_FALSE(client.Get(ep4, "/ping").isError);

  Counters c0;
  Counters c1;
  Counters c2;
  Counters c3;
  Counters c4;
  std::atomic<bool> stop{false};

  // Start sender threads: 3 sync + 1 async per server = 20 threads total.
  std::vector<std::thread> threads;
  auto startSenders = [&](const chttp2::Endpoint& ep, Counters& ctr) {
    for (int t = 0; t < 3; t++) {
      threads.emplace_back(
          senderLoop, std::ref(client), std::cref(ep), std::ref(ctr), std::ref(stop));
    }
    threads.emplace_back(
        asyncBurstLoop, std::ref(client), std::cref(ep), std::ref(ctr), std::ref(stop));
  };
  startSenders(ep0, c0);
  startSenders(ep1, c1);
  startSenders(ep2, c2);
  startSenders(ep3, c3);
  startSenders(ep4, c4);

  // =========================================================================
  // Phase 0: steady-state warm-up (3s)
  // =========================================================================
  std::printf("[phase 0] steady-state warm-up (3s) ...\n");
  waitFor(ms(3000));

  std::printf("[phase 0] baseline:\n");
  c0.print("s0-stable");
  c1.print("s1");
  c2.print("s2");
  c3.print("s3");
  c4.print("s4");

  // All servers should have delivered at least some successes.
  REQUIRE(c0.success.load() > 0);
  REQUIRE(c1.success.load() > 0);
  REQUIRE(c2.success.load() > 0);
  REQUIRE(c3.success.load() > 0);
  REQUIRE(c4.success.load() > 0);

  // =========================================================================
  // Phase 1: SIGKILL server 1 (immediate crash)
  // =========================================================================
  std::printf("\n[phase 1] SIGKILL server 1 ...\n");
  kill(s1.pid(), SIGKILL);
  waitFor(ms(3000));

  std::printf("[phase 1] after crash:\n");
  c1.print("s1-killed");
  REQUIRE(c1.error.load() > 0);

  // =========================================================================
  // Phase 2: GOAWAY server 2 (graceful drain)
  // =========================================================================
  std::printf("\n[phase 2] GOAWAY server 2 ...\n");
  client.Get(ep2, "/goaway");
  waitFor(ms(3000));

  std::printf("[phase 2] after goaway:\n");
  c2.print("s2-goaway");
  REQUIRE(c2.error.load() > 0);

  // =========================================================================
  // Phase 3: TCP RST server 3 (abrupt close)
  // =========================================================================
  std::printf("\n[phase 3] TCP RST server 3 ...\n");
  client.Get(ep3, "/close");
  waitFor(ms(3000));

  std::printf("[phase 3] after rst:\n");
  c3.print("s3-rst");
  REQUIRE(c3.error.load() > 0);

  // s0 (anchor) must remain healthy throughout all phases.
  {
    int total = c0.done.load();
    int success = c0.success.load();
    double rate = total > 0 ? static_cast<double>(success) / total : 0.0;
    std::printf("\n[phase 3] s0-stable rate: %.1f%% (%d/%d)\n", rate * 100, success, total);
    REQUIRE(rate > 0.95);
  }

  // =========================================================================
  // Phase 4: restart crashed/goaway'd servers, verify recovery
  // =========================================================================
  std::printf("\n[phase 4] restarting servers 1 and 2 ...\n");

  // Clean up old processes.
  s1.stop();
  s2.stop();

  // Restart.
  REQUIRE(s1.start());
  REQUIRE(s2.start());

  // The endpoints have new ports — update the senders.
  // We can't change ep1/ep2 mid-thread, so we stop senders, reset, and
  // restart with new endpoints.
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }
  threads.clear();

  ep1 = s1.endpoint();
  ep2 = s2.endpoint();

  // Verify the restarted servers are reachable.
  // Use a generous retry window — in CI the connector thread may still be
  // draining stale connect tasks from the chaos phases.
  for (int attempt = 0; attempt < 30; attempt++) {
    if (!client.Get(ep1, "/ping").isError) {
      break;
    }
    std::this_thread::sleep_for(ms(300));
  }
  REQUIRE_FALSE(client.Get(ep1, "/ping").isError);
  for (int attempt = 0; attempt < 30; attempt++) {
    if (!client.Get(ep2, "/ping").isError) {
      break;
    }
    std::this_thread::sleep_for(ms(300));
  }
  REQUIRE_FALSE(client.Get(ep2, "/ping").isError);

  // Reset counters and restart senders.
  c0.reset();
  c1.reset();
  c2.reset();
  c3.reset();
  c4.reset();
  stop.store(false);

  startSenders(ep0, c0);
  startSenders(ep1, c1);
  startSenders(ep2, c2);
  startSenders(ep3, c3);
  startSenders(ep4, c4);

  std::printf("[phase 4] senders restarted, recovering (5s) ...\n");
  waitFor(ms(5000));

  std::printf("[phase 4] post-recovery:\n");
  c0.print("s0-stable");
  c1.print("s1-restart");
  c2.print("s2-restart");
  c3.print("s3");
  c4.print("s4");

  // Restarted servers should be working.
  REQUIRE(c1.success.load() > 0);
  REQUIRE(c2.success.load() > 0);

  // =========================================================================
  // Phase 5: second wave — SIGKILL server 1 again + GOAWAY server 4
  // =========================================================================
  std::printf("\n[phase 5] second chaos wave: SIGKILL s1 + GOAWAY s4 ...\n");
  kill(s1.pid(), SIGKILL);
  client.Get(ep4, "/goaway");
  waitFor(ms(3000));

  std::printf("[phase 5] after second wave:\n");
  c1.print("s1-killed2");
  c4.print("s4-goaway");
  REQUIRE(c1.error.load() > 0);
  REQUIRE(c4.error.load() > 0);

  // =========================================================================
  // Phase 6: sustained load on surviving servers (s0, s2, s3) for 5s
  // =========================================================================
  std::printf("\n[phase 6] sustained load on survivors (5s) ...\n");
  waitFor(ms(5000));

  std::printf("[phase 6] survivors:\n");
  c0.print("s0-stable");
  c2.print("s2-alive");

  // s0 must have very high success rate across the entire run.
  {
    int total = c0.done.load();
    int success = c0.success.load();
    double rate = total > 0 ? static_cast<double>(success) / total : 0.0;
    std::printf("\n[phase 6] s0-stable rate: %.1f%% (%d/%d)\n", rate * 100, success, total);
    REQUIRE(rate > 0.95);
  }

  // s2 recovered and should be mostly healthy now.
  {
    int total = c2.done.load();
    int success = c2.success.load();
    double rate = total > 0 ? static_cast<double>(success) / total : 0.0;
    std::printf("[phase 6] s2-alive rate: %.1f%% (%d/%d)\n", rate * 100, success, total);
    REQUIRE(rate > 0.80);
  }

  // =========================================================================
  // Phase 7: stop senders, final sequential verification burst
  // =========================================================================
  stop.store(true);
  for (auto& t : threads) {
    t.join();
  }
  threads.clear();

  std::printf("\n[phase 7] final verification burst (100 requests to s0) ...\n");
  int finalSuccess = 0;
  for (int i = 0; i < 100; i++) {
    auto resp = client.Get(ep0, "/ping");
    if (!resp.isError && resp.getStatusCode() == 200) {
      finalSuccess++;
    }
  }
  std::printf("[phase 7] final burst: %d/100 success\n", finalSuccess);
  REQUIRE(finalSuccess == 100);

  // Also verify s2 (restarted, survived second wave).
  int s2Final = 0;
  for (int i = 0; i < 50; i++) {
    auto resp = client.Get(ep2, "/ping");
    if (!resp.isError && resp.getStatusCode() == 200) {
      s2Final++;
    }
  }
  std::printf("[phase 7] s2 final burst: %d/50 success\n", s2Final);
  REQUIRE(s2Final == 50);

  // =========================================================================
  // Summary
  // =========================================================================
  std::printf("\n=== Chaos gate test PASSED ===\n\n");

  client.close();
  s0.stop();
  s1.stop();
  s2.stop();
  s3.stop();
  s4.stop();
}
