#include <chrono>
#include <iostream>
#include <vector>

#include "chttp2/client.hpp"

/**
 * Simple benchmark to measure request throughput
 * Usage: ./benchmark_throughput <host> <port> <num_requests>
 */

struct BenchmarkResults {
  int totalRequests;
  int successfulRequests;
  int failedRequests;
  double totalTimeMs;
  double avgTimeMs;
  double requestsPerSecond;
};

BenchmarkResults runBenchmark(const std::string& host, uint16_t port, int numRequests) {
  chttp2::Endpoint ep;
  ep.host = host;
  ep.port = port;
  ep.useSSL = (port == 443);

  chttp2::Client client;

  BenchmarkResults results;
  results.totalRequests = numRequests;
  results.successfulRequests = 0;
  results.failedRequests = 0;

  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < numRequests; ++i) {
    auto response = client.get(ep, "/");

    if (response.is_error) {
      results.failedRequests++;
    } else {
      results.successfulRequests++;
    }

    if ((i + 1) % 100 == 0) {
      std::cout << "Completed " << (i + 1) << " requests..." << std::endl;
    }
  }

  auto end = std::chrono::high_resolution_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  results.totalTimeMs = duration.count();
  results.avgTimeMs = results.totalTimeMs / numRequests;
  results.requestsPerSecond = (numRequests * 1000.0) / results.totalTimeMs;

  return results;
}

void printResults(const BenchmarkResults& results) {
  std::cout << "\n=== Benchmark Results ===" << std::endl;
  std::cout << "Total requests:      " << results.totalRequests << std::endl;
  std::cout << "Successful:          " << results.successfulRequests << std::endl;
  std::cout << "Failed:              " << results.failedRequests << std::endl;
  std::cout << "Total time:          " << results.totalTimeMs << " ms" << std::endl;
  std::cout << "Average per request: " << results.avgTimeMs << " ms" << std::endl;
  std::cout << "Requests/second:     " << results.requestsPerSecond << std::endl;
}

int main(int argc, char* argv[]) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0] << " <host> <port> <num_requests>" << std::endl;
    std::cerr << "Example: " << argv[0] << " localhost 8080 1000" << std::endl;
    return 1;
  }

  std::string host = argv[1];
  uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
  int numRequests = std::stoi(argv[3]);

  std::cout << "Benchmarking " << host << ":" << port << std::endl;
  std::cout << "Running " << numRequests << " requests..." << std::endl;

  auto results = runBenchmark(host, port, numRequests);
  printResults(results);

  return 0;
}
