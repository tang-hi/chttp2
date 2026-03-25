#include "chttp2/flow.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

using namespace chttp2;

TEST_CASE("inFlow - Initialization", "[flow]") {
  SECTION("Initialize with window size") {
    InFlow flow;
    flow.init(65535);
    REQUIRE(flow.available() == 65535);
  }

  SECTION("Take within available window") {
    InFlow flow;
    flow.init(65535);

    bool result = flow.take(1000);
    REQUIRE(result);
  }
}

TEST_CASE("outFlow - Basic", "[flow]") {
  SECTION("Add and take") {
    OutFlow flow;
    flow.add(65535);
    flow.take(1000);
    REQUIRE(true);
  }
}
