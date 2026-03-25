#include "chttp2/byte_buffer.hpp"
#include "chttp2/byte_span.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

// ===========================================================================
// ByteBuffer Tests
// ===========================================================================

TEST_CASE("ByteBuffer - Basic Operations", "[byte_buffer]") {
  SECTION("Default construction") {
    chttp2::ByteBuffer buf;
    REQUIRE(buf.empty());
    REQUIRE(buf.size() == 0);
  }

  SECTION("Create with capacity") {
    chttp2::ByteBuffer buf(100u);
    REQUIRE(buf.empty());
    REQUIRE(buf.capacity() >= 100);
  }

  SECTION("Create from container") {
    std::string str = "Hello, World!";
    auto buf = chttp2::ByteBuffer::from(str);
    REQUIRE(buf.size() == str.size());
    REQUIRE_FALSE(buf.empty());
    REQUIRE(std::equal(buf.begin(), buf.end(), str.begin()));
  }
}

TEST_CASE("ByteBuffer - Append and Read", "[byte_buffer]") {
  SECTION("Append primitives and read via span") {
    chttp2::ByteBuffer buf;

    buf.append<uint32_t>(42);
    buf.append<uint8_t>(255);

    chttp2::ByteSpan span = buf;
    REQUIRE(span.readAs<uint32_t>() == 42);
    REQUIRE(span.readAs<uint8_t>() == 255);
  }

  SECTION("Peek without advancing cursor") {
    chttp2::ByteBuffer buf;
    buf.append<uint32_t>(42);

    chttp2::ByteSpan span = buf;
    REQUIRE(span.peek<uint32_t>() == 42);
    REQUIRE(span.peek<uint32_t>() == 42);  // Cursor shouldn't move
    REQUIRE(span.size() == sizeof(uint32_t));
  }

  SECTION("Write at specific position") {
    chttp2::ByteBuffer buf;
    buf.append<uint32_t>(0);
    buf.writeAt<uint32_t>(42, 0);

    chttp2::ByteSpan span = buf;
    REQUIRE(span.readAs<uint32_t>() == 42);
  }
}

TEST_CASE("ByteBuffer - Move Semantics", "[byte_buffer]") {
  SECTION("Move construction") {
    chttp2::ByteBuffer original;
    original.append<uint32_t>(42);

    auto moved = std::move(original);
    REQUIRE(moved.size() == sizeof(uint32_t));

    chttp2::ByteSpan span = moved;
    REQUIRE(span.readAs<uint32_t>() == 42);
  }

  SECTION("Move assignment") {
    chttp2::ByteBuffer buf1;
    buf1.append<uint32_t>(42);

    chttp2::ByteBuffer buf2;
    buf2 = std::move(buf1);
    REQUIRE(buf2.size() == sizeof(uint32_t));

    chttp2::ByteSpan span = buf2;
    REQUIRE(span.readAs<uint32_t>() == 42);
  }
}

TEST_CASE("ByteBuffer - Comparison Operations", "[byte_buffer]") {
  SECTION("Equal buffers") {
    std::string str = "test";
    auto buf1 = chttp2::ByteBuffer::from(str);
    auto buf2 = chttp2::ByteBuffer::from(str);
    REQUIRE(buf1 == buf2);
  }

  SECTION("Different buffers") {
    auto buf1 = chttp2::ByteBuffer::from(std::string("test1"));
    auto buf2 = chttp2::ByteBuffer::from(std::string("test2"));
    REQUIRE(buf1 != buf2);
  }
}

TEST_CASE("ByteBuffer - Edge Cases", "[byte_buffer]") {
  SECTION("Zero capacity creation") {
    chttp2::ByteBuffer buf(0u);
    REQUIRE(buf.empty());
    REQUIRE_NOTHROW(buf.append<uint8_t>(1));  // Should auto-resize
  }

  SECTION("Append near capacity boundary") {
    chttp2::ByteBuffer buf(4u);
    REQUIRE_NOTHROW(buf.append<uint32_t>(0xFFFFFFFF));
    REQUIRE_NOTHROW(buf.append<uint8_t>(0xFF));  // Should trigger resize
    REQUIRE(buf.size() == 5);
  }

  SECTION("Multiple resizes") {
    chttp2::ByteBuffer buf(1u);
    std::vector<uint8_t> data(100, 0xFF);
    REQUIRE_NOTHROW(buf.append(data.begin(), data.end()));
    REQUIRE(buf.size() == 100);
  }
}

TEST_CASE("ByteBuffer - Iterator Operations", "[byte_buffer]") {
  SECTION("Iterator consistency") {
    std::string testStr = "test string";
    auto buf = chttp2::ByteBuffer::from(testStr);
    long length = static_cast<long>(testStr.size());
    REQUIRE(std::distance(buf.begin(), buf.end()) == length);
    REQUIRE(std::equal(buf.begin(), buf.end(), testStr.begin()));
  }
}

TEST_CASE("ByteBuffer - Advanced Operations", "[byte_buffer]") {
  SECTION("Mixed read/write operations") {
    chttp2::ByteBuffer buf;
    buf.append<uint16_t>(0x1234);
    buf.append<uint8_t>(0x56);
    buf.append<uint8_t>(0x78);

    chttp2::ByteSpan span = buf;
    REQUIRE(span.readAs<uint16_t>() == 0x1234);
    REQUIRE(span.offset() == 2);
    buf.writeAt<uint8_t>(0x99, 2);

    // Re-read from updated buffer
    chttp2::ByteSpan span2 = buf;
    span2.advance(2);
    REQUIRE(span2.readAs<uint8_t>() == 0x99);
  }
}

// ===========================================================================
// ByteSpan Tests
// ===========================================================================

TEST_CASE("ByteSpan - Basic Operations", "[byte_span]") {
  SECTION("Default construction") {
    chttp2::ByteSpan span;
    REQUIRE(span.empty());
    REQUIRE(span.size() == 0);
  }

  SECTION("From ByteBuffer") {
    chttp2::ByteBuffer buf;
    buf.append<uint32_t>(42);

    chttp2::ByteSpan span = buf;
    REQUIRE(span.size() == sizeof(uint32_t));
    REQUIRE_FALSE(span.empty());
  }
}

TEST_CASE("ByteSpan - Subspan", "[byte_span]") {
  SECTION("Subspan with offset and count") {
    std::string str = "Hello, World!";
    auto buf = chttp2::ByteBuffer::from(str);
    chttp2::ByteSpan span = buf;

    auto sub = span.subspan(0, 5);
    REQUIRE(sub.size() == 5);
  }

  SECTION("Subspan with offset only") {
    std::string str = "Hello, World!";
    auto buf = chttp2::ByteBuffer::from(str);
    chttp2::ByteSpan span = buf;

    auto sub = span.subspan(7);
    REQUIRE(sub.size() == 6);  // "World!"
  }

  SECTION("Nested subspans") {
    auto buf = chttp2::ByteBuffer::from(std::string("0123456789"));
    chttp2::ByteSpan span = buf;
    auto sub1 = span.subspan(2, 6);  // "234567"
    auto sub2 = sub1.subspan(2, 2);  // "45"
    REQUIRE(sub2.size() == 2);
    REQUIRE(sub2[0] == '4');
    REQUIRE(sub2[1] == '5');
  }

  SECTION("Boundary operations") {
    auto buf = chttp2::ByteBuffer::from(std::string("test"));
    chttp2::ByteSpan span = buf;
    REQUIRE_NOTHROW(span.subspan(0, span.size()));
    REQUIRE_NOTHROW(span.subspan(span.size(), 0));
    REQUIRE_THROWS_AS(span.subspan(0, span.size() + 1), std::runtime_error);
    REQUIRE_THROWS_AS(span.subspan(span.size() + 1, 0), std::runtime_error);
  }
}

TEST_CASE("ByteSpan - Error Handling", "[byte_span]") {
  SECTION("Out of bounds access") {
    chttp2::ByteSpan span;
    REQUIRE_THROWS_AS(span[0], std::runtime_error);
    REQUIRE_THROWS_AS(span.readAs<uint32_t>(), std::runtime_error);
    REQUIRE_THROWS_AS(span.advance(1), std::runtime_error);
  }

  SECTION("Invalid subspan") {
    chttp2::ByteSpan span;
    REQUIRE_THROWS_AS(span.subspan(1), std::runtime_error);
    REQUIRE_THROWS_AS(span.subspan(0, 1), std::runtime_error);
  }
}

TEST_CASE("ByteSpan - Cursor Independence", "[byte_span]") {
  SECTION("Copied spans have independent cursors") {
    chttp2::ByteBuffer buf;
    buf.append<uint32_t>(42);
    buf.append<uint32_t>(99);

    chttp2::ByteSpan span1 = buf;
    chttp2::ByteSpan span2 = span1;

    REQUIRE(span1.readAs<uint32_t>() == 42);
    REQUIRE(span2.readAs<uint32_t>() == 42);  // Independent cursor
    REQUIRE(span1.readAs<uint32_t>() == 99);
    REQUIRE(span2.readAs<uint32_t>() == 99);
  }
}

TEST_CASE("ByteSpan - Iterator Operations", "[byte_span]") {
  SECTION("Iterator consistency") {
    std::string testStr = "test string";
    auto buf = chttp2::ByteBuffer::from(testStr);
    chttp2::ByteSpan span = buf;
    long length = static_cast<long>(testStr.size());
    REQUIRE(std::distance(span.begin(), span.end()) == length);
    REQUIRE(std::equal(span.begin(), span.end(), testStr.begin()));
  }

  SECTION("Iterator after advance") {
    auto buf = chttp2::ByteBuffer::from(std::string("0123456789"));
    chttp2::ByteSpan span = buf;
    span.advance(5);
    REQUIRE(std::distance(span.begin(), span.end()) == 5);
    REQUIRE(*span.begin() == '5');
  }
}

TEST_CASE("ByteSpan - Comparison", "[byte_span]") {
  SECTION("Equal spans") {
    auto buf1 = chttp2::ByteBuffer::from(std::string("test"));
    auto buf2 = chttp2::ByteBuffer::from(std::string("test"));
    chttp2::ByteSpan span1 = buf1;
    chttp2::ByteSpan span2 = buf2;
    REQUIRE(span1 == span2);
  }

  SECTION("Different spans") {
    auto buf1 = chttp2::ByteBuffer::from(std::string("test1"));
    auto buf2 = chttp2::ByteBuffer::from(std::string("test2"));
    chttp2::ByteSpan span1 = buf1;
    chttp2::ByteSpan span2 = buf2;
    REQUIRE(span1 != span2);
  }
}

// ===========================================================================
// Fuzz Tests
// ===========================================================================

TEST_CASE("ByteBuffer/ByteSpan - Fuzz Testing", "[byte_buffer][byte_span][fuzz]") {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);

  SECTION("Random data operations") {
    std::vector<uint8_t> testData(1000);
    for (auto& byte : testData) {
      byte = static_cast<uint8_t>(dis(gen));
    }

    chttp2::ByteBuffer buf;

    for (size_t i = 0; i < 100; i++) {
      long chunkSize = static_cast<long>((dis(gen) % 10) + 1);
      auto start = testData.begin() +
                   (static_cast<long>(dis(gen)) % (static_cast<long>(testData.size()) - chunkSize));

      buf.clear();
      buf.append(start, start + chunkSize);
      REQUIRE(buf.size() == static_cast<uint32_t>(chunkSize));
      REQUIRE(std::equal(buf.begin(), buf.end(), start));
    }
  }

  SECTION("Random subspan operations") {
    std::vector<uint8_t> testData(100);
    for (auto& byte : testData) {
      byte = static_cast<uint8_t>(dis(gen));
    }

    auto buf = chttp2::ByteBuffer::from(testData);
    chttp2::ByteSpan span = buf;

    for (size_t i = 0; i < 50; i++) {
      uint32_t off = static_cast<uint32_t>(dis(gen)) % span.size();
      uint32_t length = static_cast<uint32_t>(dis(gen)) % (span.size() - off);

      auto sub = span.subspan(off, length);
      REQUIRE(sub.size() == length);
      REQUIRE(std::equal(sub.begin(), sub.end(), testData.begin() + off));
    }
  }
}
