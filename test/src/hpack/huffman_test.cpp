#include "chttp2/hpack/huffman.hpp"

#include <catch2/catch_test_macros.hpp>
#include <random>
#include <string>

#include "utils.hpp"

TEST_CASE("huffman RFC vectors", "[huffman]") {
  chttp2::Huffman huffman;
  std::vector<std::string> encoded = {
      "f1e3 c2e5 f23a 6ba0 ab90 f4ff",
      "a8eb 1064 9cbf",
      "25a8 49e9 5ba9 7d7f",
      "25a8 49e9 5bb8 e8b4 bf",
      "6402",
      "aec3 771a 4b",
      "d07a be94 1054 d444 a820 0595 040b 8166 e082 a62d 1bff",
      "9d29 ad17 1863 c78f 0b97 c8e9 ae82 ae43 d3",
      "9bd9 ab",
      R"(94e7 821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed 4ee5 b106 3d50 07)"};

  std::vector<std::string> decoded = {"www.example.com",
                                      "no-cache",
                                      "custom-key",
                                      "custom-value",
                                      "302",
                                      "private",
                                      "Mon, 21 Oct 2013 20:13:21 GMT",
                                      "https://www.example.com",
                                      "gzip",
                                      "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"};

  SECTION("decode") {
    for (std::size_t i = 0; i < encoded.size(); i++) {
      std::string result;
      auto code = huffman.decode(chttp2::ByteBuffer::from(hexToBytes(encoded[i])), result);
      REQUIRE(code == chttp2::HuffmanResult::OK);
      REQUIRE(result == decoded[i]);
    }
  }

  SECTION("encode") {
    for (std::size_t i = 0; i < decoded.size(); i++) {
      auto result = huffman.encode(decoded[i]);
      REQUIRE(result == chttp2::ByteBuffer::from(hexToBytes(encoded[i])));
    }
  }
}

TEST_CASE("huffman edge cases", "[huffman]") {
  chttp2::Huffman huffman;

  SECTION("empty string") {
    auto encoded = huffman.encode("");
    REQUIRE(encoded.empty());
    REQUIRE(huffman.encodedLength("") == 0);

    std::string decoded;
    REQUIRE(huffman.decode(encoded, decoded) == chttp2::HuffmanResult::OK);
    REQUIRE(decoded.empty());
  }

  SECTION("single character") {
    for (auto c : {'0', 'a', 'Z', ' ', '~'}) {
      std::string input(1, c);
      auto encoded = huffman.encode(input);
      REQUIRE(encoded.size() == huffman.encodedLength(input));

      std::string decoded;
      REQUIRE(huffman.decode(encoded, decoded) == chttp2::HuffmanResult::OK);
      REQUIRE(decoded == input);
    }
  }

  SECTION("all 256 byte values round-trip") {
    std::string allBytes;
    for (int i = 0; i < 256; ++i) {
      allBytes += static_cast<char>(i);
    }
    auto encoded = huffman.encode(allBytes);
    REQUIRE(encoded.size() == huffman.encodedLength(allBytes));

    std::string decoded;
    REQUIRE(huffman.decode(encoded, decoded) == chttp2::HuffmanResult::OK);
    REQUIRE(decoded == allBytes);
  }
}

TEST_CASE("huffman error cases", "[huffman]") {
  chttp2::Huffman huffman;

  SECTION("invalid padding") {
    std::vector<std::vector<uint8_t>> cases = {
        {0xff},                                    // padding exceeds 7 bits
        {0x1f, 0xff},                              // exceeds 1 byte
        {0x1f, 0xff, 0xff},                        // exceeds 2 bytes
        {0x1f, 0xff, 0xff, 0xff},                  // exceeds 3 bytes
        {0xff, 0x9f, 0xff, 0xff, 0xff},            // exceeds 29 bits
        {'R', 0xbc, '0', 0xff, 0xff, 0xff, 0xff},  // padding ends on partial symbol
    };
    for (auto& e : cases) {
      std::string result;
      REQUIRE(huffman.decode(chttp2::ByteBuffer::from(e), result) ==
              chttp2::HuffmanResult::INVALID_CODE);
    }
  }

  SECTION("full EOS symbol") {
    std::vector<uint8_t> eos = {0xff, 0xff, 0xff, 0xff, 0xfc};
    std::string result;
    REQUIRE(huffman.decode(chttp2::ByteBuffer::from(eos), result) ==
            chttp2::HuffmanResult::INVALID_CODE);
  }

  SECTION("corrupt padding (not all 1s)") {
    std::string result;
    REQUIRE(huffman.decode(chttp2::ByteBuffer::from(std::vector<uint8_t>{0x00}), result) ==
            chttp2::HuffmanResult::INVALID_CODE);
  }
}

TEST_CASE("huffman fuzz", "[huffman]") {
  chttp2::Huffman huffman;

  SECTION("round-trip with fixed seeds") {
    for (uint32_t seed : {1u, 42u, 123u, 456u, 789u, 2024u, 9999u, 31415u}) {
      std::mt19937 rng(seed);
      std::uniform_int_distribution<int> dist(0, 255);

      std::string original;
      for (int i = 0; i < 1000; ++i) {
        original += static_cast<char>(dist(rng));
      }

      auto encoded = huffman.encode(original);
      REQUIRE(encoded.size() == huffman.encodedLength(original));

      std::string decoded;
      REQUIRE(huffman.decode(encoded, decoded) == chttp2::HuffmanResult::OK);
      REQUIRE(original == decoded);
    }
  }

  SECTION("random bytes decode doesn't crash") {
    int failed = 0;
    for (uint32_t seed : {1u, 42u, 123u, 456u, 789u, 2024u, 9999u, 31415u}) {
      std::mt19937 rng(seed);
      std::uniform_int_distribution<int> dist(0, 255);

      chttp2::ByteBuffer buf;
      for (int i = 0; i < 50; ++i) {
        buf.append(static_cast<uint8_t>(dist(rng)));
      }

      std::string decoded;
      if (huffman.decode(buf, decoded) != chttp2::HuffmanResult::OK) {
        ++failed;
      }
    }
    REQUIRE(failed > 0);
  }
}
