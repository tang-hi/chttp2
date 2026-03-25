// chttp2 client performance benchmark.
// Run with: make bench
//
// Usage:
//   chttp2_bench                         # run all scenarios
//   chttp2_bench sequential concurrent   # run selected scenarios
//   chttp2_bench --list                  # list available scenarios
//
// Scenario names:
//   sequential  concurrent  multithread
//   1kb  64kb  1mb  upload  mixed

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "chttp2/client.hpp"
#include "chttp2/endpoint.hpp"
#include "chttp2/response.hpp"
#include "e2e_fixture.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct Percentiles {
  double p50, p90, p99, max, min, avg;
};

Percentiles computePercentiles(std::vector<double>& samples) {
  Percentiles p{};
  if (samples.empty()) {
    return p;
  }
  std::sort(samples.begin(), samples.end());
  std::size_t n = samples.size();
  p.min = samples[0];
  p.max = samples[n - 1];
  p.p50 = samples[n * 50 / 100];
  p.p90 = samples[n * 90 / 100];
  p.p99 = samples[std::min(n - 1, n * 99 / 100)];
  double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  p.avg = sum / static_cast<double>(n);
  return p;
}

struct BenchResult {
  int requests;
  int success;
  double durationSec;
  int64_t totalBytes;
  std::vector<double> latenciesMs;
};

void printResult(const char* name, BenchResult& result) {
  double rps = result.requests / result.durationSec;
  auto pct = computePercentiles(result.latenciesMs);

  std::printf("\n--- %s ---\n", name);
  std::printf("  %d requests in %.1fs  (%d ok, %d err)\n",
              result.requests, result.durationSec,
              result.success, result.requests - result.success);
  std::printf("  throughput: %.1f req/s\n", rps);
  std::printf("  latency:   p50=%.2fms  p90=%.2fms  p99=%.2fms  max=%.2fms\n",
              pct.p50, pct.p90, pct.p99, pct.max);

  if (result.totalBytes > 0) {
    double mbps = (static_cast<double>(result.totalBytes) / (1024.0 * 1024.0)) / result.durationSec;
    std::printf("  data rate: %.1f MB/s\n", mbps);
  }
}

// ============================================================================
// Scenario: sequential requests from a single thread
// ============================================================================

BenchResult runSequential(chttp2::Client& client,
                          const chttp2::Endpoint& ep,
                          const char* path,
                          const std::string& body,
                          int durationSec,
                          int expectedBodySize) {
  BenchResult result{};
  result.totalBytes = 0;

  auto deadline = Clock::now() + std::chrono::seconds(durationSec);
  auto benchStart = Clock::now();

  while (Clock::now() < deadline) {
    auto reqStart = Clock::now();
    chttp2::Response resp;
    if (body.empty()) {
      resp = client.Get(ep, path);
    } else {
      resp = client.Post(ep, path, body);
    }
    double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - reqStart).count();
    result.latenciesMs.push_back(elapsedMs);
    result.requests++;
    if (!resp.isError && resp.getStatusCode() >= 200 && resp.getStatusCode() < 300) {
      result.success++;
      if (expectedBodySize > 0) {
        result.totalBytes += static_cast<int64_t>(resp.body.size());
      }
    }
  }

  result.durationSec = std::chrono::duration<double>(Clock::now() - benchStart).count();
  return result;
}

// ============================================================================
// Scenario: N concurrent async streams
// ============================================================================

BenchResult runConcurrentStreams(chttp2::Client& client,
                                const chttp2::Endpoint& ep,
                                int maxInFlight,
                                int durationSec) {
  BenchResult result{};
  std::atomic<int> inFlight{0};
  std::mutex latMutex;
  std::atomic<int> done{0};
  std::atomic<int> success{0};
  std::vector<double> latencies;
  latencies.reserve(static_cast<std::size_t>(maxInFlight * durationSec) * 200);

  auto deadline = Clock::now() + std::chrono::seconds(durationSec);
  auto benchStart = Clock::now();

  while (Clock::now() < deadline) {
    while (inFlight.load(std::memory_order_relaxed) < maxInFlight && Clock::now() < deadline) {
      auto reqStart = Clock::now();
      inFlight.fetch_add(1, std::memory_order_relaxed);
      client.Get(ep, "/ping", [&, reqStart](const chttp2::Response& r) {
        double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - reqStart).count();
        {
          std::lock_guard<std::mutex> lock(latMutex);
          latencies.push_back(elapsedMs);
        }
        if (!r.isError && r.getStatusCode() == 200) {
          success.fetch_add(1, std::memory_order_relaxed);
        }
        done.fetch_add(1, std::memory_order_relaxed);
        inFlight.fetch_add(-1, std::memory_order_relaxed);
      });
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }

  // Drain remaining in-flight.
  auto drainDeadline = Clock::now() + std::chrono::seconds(10);
  while (inFlight.load() > 0 && Clock::now() < drainDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  result.durationSec = std::chrono::duration<double>(Clock::now() - benchStart).count();
  result.requests = done.load();
  result.success = success.load();
  {
    std::lock_guard<std::mutex> lock(latMutex);
    result.latenciesMs = std::move(latencies);
  }
  return result;
}

// ============================================================================
// Scenario: multi-thread sequential
// ============================================================================

BenchResult runMultiThread(chttp2::Client& client,
                           const chttp2::Endpoint& ep,
                           std::size_t numThreads,
                           int durationSec) {
  struct ThreadResult {
    int requests{0};
    int success{0};
    std::vector<double> latencies;
  };

  std::vector<ThreadResult> perThread(numThreads);
  auto deadline = Clock::now() + std::chrono::seconds(durationSec);
  auto benchStart = Clock::now();

  std::vector<std::thread> threads;
  threads.reserve(numThreads);

  for (std::size_t t = 0; t < numThreads; t++) {
    threads.emplace_back([&client, &ep, &perThread, t, deadline]() {
      auto& tr = perThread[t];
      tr.latencies.reserve(10000);
      while (Clock::now() < deadline) {
        auto reqStart = Clock::now();
        auto resp = client.Get(ep, "/ping");
        double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - reqStart).count();
        tr.latencies.push_back(elapsedMs);
        tr.requests++;
        if (!resp.isError && resp.getStatusCode() == 200) {
          tr.success++;
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  BenchResult result{};
  result.durationSec = std::chrono::duration<double>(Clock::now() - benchStart).count();
  for (auto& tr : perThread) {
    result.requests += tr.requests;
    result.success += tr.success;
    result.latenciesMs.insert(result.latenciesMs.end(), tr.latencies.begin(), tr.latencies.end());
  }
  return result;
}

// ============================================================================
// Scenario: mixed workload (multi-thread, varied request types)
// ============================================================================

BenchResult runMixed(chttp2::Client& client,
                     const chttp2::Endpoint& ep,
                     std::size_t numThreads,
                     int durationSec) {
  struct ThreadResult {
    int requests{0};
    int success{0};
    int64_t totalBytes{0};
    std::vector<double> latencies;
  };

  std::vector<ThreadResult> perThread(numThreads);
  auto deadline = Clock::now() + std::chrono::seconds(durationSec);
  auto benchStart = Clock::now();
  std::string uploadBody(4096, 'B');

  std::vector<std::thread> threads;
  threads.reserve(numThreads);

  for (std::size_t t = 0; t < numThreads; t++) {
    threads.emplace_back([&client, &ep, &perThread, &uploadBody, t, deadline]() {
      auto& tr = perThread[t];
      tr.latencies.reserve(5000);
      int i = 0;
      while (Clock::now() < deadline) {
        auto reqStart = Clock::now();
        chttp2::Response resp;
        switch (i % 5) {
          case 0:
            resp = client.Get(ep, "/ping");
            break;
          case 1:
            resp = client.Post(ep, "/echo-body", "bench-mixed");
            break;
          case 2:
            resp = client.Get(ep, "/big?size=8192");
            break;
          case 3:
            resp = client.Post(ep, "/upload", uploadBody);
            break;
          case 4:
            resp = client.Get(ep, "/status/201");
            break;
          default:
            resp = client.Get(ep, "/ping");
            break;
        }
        double elapsedMs =
            std::chrono::duration<double, std::milli>(Clock::now() - reqStart).count();
        tr.latencies.push_back(elapsedMs);
        tr.requests++;
        if (!resp.isError && resp.getStatusCode() >= 200 && resp.getStatusCode() < 300) {
          tr.success++;
          tr.totalBytes += static_cast<int64_t>(resp.body.size());
        }
        i++;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  BenchResult result{};
  result.durationSec = std::chrono::duration<double>(Clock::now() - benchStart).count();
  for (auto& tr : perThread) {
    result.requests += tr.requests;
    result.success += tr.success;
    result.totalBytes += tr.totalBytes;
    result.latenciesMs.insert(result.latenciesMs.end(), tr.latencies.begin(), tr.latencies.end());
  }
  return result;
}

// ============================================================================
// Scenario registry
// ============================================================================

struct Scenario {
  const char* name;
  const char* description;
  void (*run)(chttp2::Client& client, const chttp2::Endpoint& ep, int duration);
};

void scenarioSequential(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  auto r = runSequential(c, ep, "/ping", "", d, 0);
  printResult("Sequential GET /ping (baseline)", r);
}

void scenarioConcurrent(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  auto r = runConcurrentStreams(c, ep, 100, d);
  printResult("100 concurrent streams GET /ping", r);
}

void scenarioMultithread(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  auto r = runMultiThread(c, ep, 8, d);
  printResult("8 threads x sequential GET /ping", r);
}

void scenario1kb(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  auto r = runSequential(c, ep, "/big?size=1024", "", d, 1024);
  printResult("Sequential GET 1KB", r);
}

void scenario64kb(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  auto r = runSequential(c, ep, "/big?size=65536", "", d, 65536);
  printResult("Sequential GET 64KB", r);
}

void scenario1mb(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  auto r = runSequential(c, ep, "/big?size=1048576", "", d, 1048576);
  printResult("Sequential GET 1MB", r);
}

void scenarioUpload(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  std::string body(65536, 'A');
  auto r = runSequential(c, ep, "/upload", body, d, 0);
  printResult("Sequential POST 64KB upload", r);
}

void scenarioMixed(chttp2::Client& c, const chttp2::Endpoint& ep, int d) {
  auto r = runMixed(c, ep, 4, d);
  printResult("Mixed workload (4 threads)", r);
}

const Scenario SCENARIOS[] = {
    {"sequential",  "Sequential GET /ping baseline latency",   scenarioSequential},
    {"concurrent",  "100 concurrent async streams",            scenarioConcurrent},
    {"multithread", "8 threads x sequential GET /ping",        scenarioMultithread},
    {"1kb",         "Sequential GET 1KB download",             scenario1kb},
    {"64kb",        "Sequential GET 64KB download",            scenario64kb},
    {"1mb",         "Sequential GET 1MB download",             scenario1mb},
    {"upload",      "Sequential POST 64KB upload",             scenarioUpload},
    {"mixed",       "Mixed workload (4 threads, 5 req types)", scenarioMixed},
};

constexpr std::size_t NUM_SCENARIOS = sizeof(SCENARIOS) / sizeof(SCENARIOS[0]);

bool shouldRun(const char* name, int argc, char** argv) {
  if (argc <= 1) {
    return true;  // No filter — run all.
  }
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], name) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  // --list: print available scenarios and exit.
  if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
    std::printf("Available scenarios:\n");
    for (const auto & i : SCENARIOS) {
      std::printf("  %-12s  %s\n", i.name, i.description);
    }
    return 0;
  }

  constexpr int kDuration = 5;  // seconds per scenario

  GoServerProcess server;
  if (!server.start()) {
    std::fprintf(stderr, "Failed to start Go test server\n");
    return 1;
  }
  auto ep = server.endpoint();

  chttp2::Client client;

  // Warm up connection.
  for (int i = 0; i < 50; i++) {
    auto resp = client.Get(ep, "/ping");
    if (resp.isError) {
      std::fprintf(stderr, "Warm-up failed\n");
      return 1;
    }
  }

  std::printf("\n=== chttp2 benchmark ===\n");
  std::printf("Server: Go h2c on 127.0.0.1:%d\n", server.port());
  std::printf("Duration: %ds per scenario\n", kDuration);

  int ran = 0;
  for (const auto & i : SCENARIOS) {
    if (shouldRun(i.name, argc, argv)) {
      i.run(client, ep, kDuration);
      ran++;
    }
  }

  if (ran == 0) {
    std::fprintf(stderr, "No matching scenarios. Use --list to see available names.\n");
    client.close();
    server.stop();
    return 1;
  }

  std::printf("\n=== benchmark complete (%d scenario%s) ===\n\n", ran, ran > 1 ? "s" : "");

  client.close();
  server.stop();
  return 0;
}
