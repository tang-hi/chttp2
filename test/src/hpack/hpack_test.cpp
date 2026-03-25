#include "chttp2/hpack/hpack.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "chttp2/byte_buffer.hpp"
#include "chttp2/byte_span.hpp"

#include "utils.hpp"

// for testing
namespace Catch {
template <>
struct StringMaker<chttp2::HeaderField> {
  static std::string convert(const chttp2::HeaderField& value) { return value.str(); }
};
}  // namespace Catch

TEST_CASE("hpack test", "[decode_single]") {
  chttp2::Hpack hpack;

  SECTION("C.2.1", "Literal Header Field With Indexding") {
    std::vector<std::uint8_t> data = {0x40, 0x0a, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d,
                                      0x6b, 0x65, 0x79, 0x0d, 0x63, 0x75, 0x73, 0x74, 0x6f,
                                      0x6d, 0x2d, 0x68, 0x65, 0x61, 0x64, 0x65, 0x72};

    std::vector<chttp2::HeaderField> headers;
    REQUIRE(hpack.decode(chttp2::ByteBuffer::from(data.begin(), data.end()), headers));
    REQUIRE(headers.size() == 1);
    REQUIRE(headers[0].name == "custom-key");
    REQUIRE(headers[0].value == "custom-header");
  }

  SECTION("C.2.2", "Literal Header Field Without Indexing") {
    std::vector<std::uint8_t> data = {
        0x04, 0x0c, 0x2f, 0x73, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2f, 0x70, 0x61, 0x74, 0x68};

    std::vector<chttp2::HeaderField> headers;
    REQUIRE(hpack.decode(chttp2::ByteBuffer::from(data.begin(), data.end()), headers));
    REQUIRE(headers.size() == 1);
    REQUIRE(headers[0].name == ":path");
    REQUIRE(headers[0].value == "/sample/path");
  }

  SECTION("C.2.3", "Literal Header Field Never Indexed") {
    std::vector<std::uint8_t> data = {0x10,
                                      0x08,
                                      0x70,
                                      0x61,
                                      0x73,
                                      0x73,
                                      0x77,
                                      0x6f,
                                      0x72,
                                      0x64,
                                      0x06,
                                      0x73,
                                      0x65,
                                      0x63,
                                      0x72,
                                      0x65,
                                      0x74};

    std::vector<chttp2::HeaderField> headers;
    REQUIRE(hpack.decode(chttp2::ByteBuffer::from(data.begin(), data.end()), headers));
    REQUIRE(headers.size() == 1);
    REQUIRE(headers[0].name == "password");
    REQUIRE(headers[0].value == "secret");
    REQUIRE(headers[0].sensitivity == true);
  }

  SECTION("C.2.4", "Indexed Header Field") {
    std::vector<std::uint8_t> data = {0x82};

    std::vector<chttp2::HeaderField> headers;
    REQUIRE(hpack.decode(chttp2::ByteBuffer::from(data.begin(), data.end()), headers));
    REQUIRE(headers.size() == 1);
    REQUIRE(headers[0].name == ":method");
    REQUIRE(headers[0].value == "GET");
  }

  SECTION("empty header block") {
    std::vector<chttp2::HeaderField> headers;
    REQUIRE(hpack.decode(chttp2::ByteSpan{}, headers));
    REQUIRE(headers.empty());
  }

  SECTION("decode errors") {
    std::vector<chttp2::HeaderField> h;
    REQUIRE_FALSE(hpack.decode(chttp2::ByteBuffer::from(hexToBytes("80")), h));  // index 0
    REQUIRE_FALSE(hpack.decode(chttp2::ByteBuffer::from(hexToBytes("bf")), h));  // index 63 OOB
    REQUIRE_FALSE(hpack.decode(chttp2::ByteBuffer::from(hexToBytes("ff")), h));  // truncated varint
    REQUIRE_FALSE(
        hpack.decode(chttp2::ByteBuffer::from(hexToBytes("4005 6162")), h));  // truncated string
    REQUIRE_FALSE(
        hpack.decode(chttp2::ByteBuffer::from(hexToBytes("82 20")), h));  // size update mid-block
  }

  SECTION("table size update exceeds limit") {
    hpack.setDecodeTableLimit(64);
    std::vector<chttp2::HeaderField> h;
    // size=128: varint(5, 128) = 0x3f 0x61, exceeds limit 64
    REQUIRE_FALSE(hpack.decode(chttp2::ByteBuffer::from(hexToBytes("3f 61")), h));
  }

  SECTION("wire-level table size update") {
    std::vector<chttp2::HeaderField> headers;
    // table size update to 0 (0x20), then :method GET (0x82)
    REQUIRE(hpack.decode(chttp2::ByteBuffer::from(hexToBytes("20 82")), headers));
    REQUIRE(headers.size() == 1);
    REQUIRE(headers[0] == chttp2::HeaderField{":method", "GET"});
  }
}

TEST_CASE("hpack test", "[RFC EXAMPLES]") {
  chttp2::Hpack hpack;
  SECTION("C3", "Request Examples without Huffman Coding") {
    using ENCODE_AND_DECODE =
        std::pair<std::vector<std::uint8_t>, std::vector<chttp2::HeaderField>>;
    std::vector<ENCODE_AND_DECODE> examples = {
        {{0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65,
          0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d},
         {{":authority", "www.example.com"},
          {":path", "/"},
          {":scheme", "http"},
          {":method", "GET"}}},
        {{0x82, 0x86, 0x84, 0xbe, 0x58, 0x08, 0x6e, 0x6f, 0x2d, 0x63, 0x61, 0x63, 0x68, 0x65},
         {{":authority", "www.example.com"},
          {":path", "/"},
          {":scheme", "http"},
          {":method", "GET"},
          {"cache-control", "no-cache"}}},
        {{0x82, 0x87, 0x85, 0xbf, 0x40, 0x0a, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x6b, 0x65,
          0x79, 0x0c, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x2d, 0x76, 0x61, 0x6c, 0x75, 0x65},
         {{":authority", "www.example.com"},
          {":path", "/index.html"},
          {":scheme", "https"},
          {":method", "GET"},
          {"custom-key", "custom-value"}}},
    };

    for (auto& example : examples) {
      std::vector<chttp2::HeaderField> headers;
      REQUIRE(
          hpack.decode(chttp2::ByteBuffer::from(example.first.begin(), example.first.end()), headers));
      std::sort(headers.begin(), headers.end());
      std::sort(example.second.begin(), example.second.end());
      REQUIRE(headers == example.second);
    }
  }

  SECTION("C4", "Request Examples with Huffman Coding") {
    using ENCODE_AND_DECODE =
        std::pair<std::vector<std::uint8_t>, std::vector<chttp2::HeaderField>>;
    std::vector<ENCODE_AND_DECODE> examples = {
        {{0x82,
          0x86,
          0x84,
          0x41,
          0x8c,
          0xf1,
          0xe3,
          0xc2,
          0xe5,
          0xf2,
          0x3a,
          0x6b,
          0xa0,
          0xab,
          0x90,
          0xf4,
          0xff},
         {{":authority", "www.example.com"},
          {":path", "/"},
          {":scheme", "http"},
          {":method", "GET"}}},
        {{0x82, 0x86, 0x84, 0xbe, 0x58, 0x86, 0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf},
         {{":authority", "www.example.com"},
          {":path", "/"},
          {":scheme", "http"},
          {":method", "GET"},
          {"cache-control", "no-cache"}}},
        {{0x82, 0x87, 0x85, 0xbf, 0x40, 0x88, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xa9,
          0x7d, 0x7f, 0x89, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xb8, 0xe8, 0xb4, 0xbf},
         {{":authority", "www.example.com"},
          {":path", "/index.html"},
          {":scheme", "https"},
          {":method", "GET"},
          {"custom-key", "custom-value"}}},
    };
    for (auto& example : examples) {
      std::vector<chttp2::HeaderField> headers;
      REQUIRE(hpack.decode(chttp2::ByteBuffer::from(example.first), headers));
      std::sort(headers.begin(), headers.end());
      std::sort(example.second.begin(), example.second.end());
      REQUIRE(headers == example.second);
    }
  }

  SECTION("C5", "Response Examples without Huffman Coding") {
    hpack.setDecodeTableLimit(256);
    using ENCODE_AND_DECODE = std::pair<std::string, std::vector<chttp2::HeaderField>>;
    std::vector<ENCODE_AND_DECODE> examples = {
        // First Response
        {"4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 "
         "7420 3230 3133 2032 303a 3133 3a32 3120 474d 546e 1768 7474 7073 "
         "3a2f 2f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
         {{":status", "302"},
          {"cache-control", "private"},
          {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
          {"location", "https://www.example.com"}}},
        // Second Response
        {"4803 3330 37c1 c0bf",
         {{":status", "307"},
          {"cache-control", "private"},
          {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
          {"location", "https://www.example.com"}}},
        // Third Response
        {"88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 "
         "3a32 3220 474d 54c0 5a04 677a 6970 7738 666f 6f3d 4153 444a 4b48 "
         "514b 425a 584f 5157 454f 5049 5541 5851 5745 4f49 553b 206d 6178 "
         "2d61 6765 3d33 3630 303b 2076 6572 7369 6f6e 3d31",
         {{":status", "200"},
          {"cache-control", "private"},
          {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
          {"location", "https://www.example.com"},
          {"content-encoding", "gzip"},
          {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"}}},
    };
    for (auto& example : examples) {
      std::vector<chttp2::HeaderField> headers;
      auto data = hexToBytes(example.first);
      REQUIRE(hpack.decode(chttp2::ByteBuffer::from(data.begin(), data.end()), headers));
      std::sort(headers.begin(), headers.end());
      std::sort(example.second.begin(), example.second.end());
      REQUIRE(headers == example.second);
    }
  }

  SECTION("C6", "Response Examples with Huffman Coding") {
    hpack.setDecodeTableLimit(256);
    using ENCODE_AND_DECODE = std::pair<std::string, std::vector<chttp2::HeaderField>>;
    std::vector<ENCODE_AND_DECODE> examples = {
        // First Response
        {"4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 "
         "0b81 66e0 82a6 2d1b ff6e 919d 29ad 1718 63c7 8f0b 97c8 e9ae 82ae "
         "43d3 ",
         {
             {":status", "302"},
             {"cache-control", "private"},
             {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
             {"location", "https://www.example.com"},
         }},
        // Second Response
        {"4883 640e ffc1 c0bf",
         {
             {":status", "307"},
             {"cache-control", "private"},
             {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
             {"location", "https://www.example.com"},
         }},
        // Third Response
        {"88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff "
         "c05a 839b d9ab 77ad 94e7 821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af "
         "2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed 4ee5 b106 3d50 07 ",
         {
             {":status", "200"},
             {"cache-control", "private"},
             {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
             {"location", "https://www.example.com"},
             {"content-encoding", "gzip"},
             {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
         }}};
    for (auto& example : examples) {
      std::vector<chttp2::HeaderField> headers;
      auto data = hexToBytes(example.first);
      REQUIRE(hpack.decode(chttp2::ByteBuffer::from(data.begin(), data.end()), headers));
      std::sort(headers.begin(), headers.end());
      std::sort(example.second.begin(), example.second.end());
      REQUIRE(headers == example.second);
    }
  }
}

TEST_CASE("hpack test", "encode") {
  chttp2::Hpack hpack;

  SECTION("encode-decode round-trip") {
    std::vector<std::vector<chttp2::HeaderField>> examples = {{
                                                                  {":method", "GET"},
                                                                  {":scheme", "http"},
                                                                  {":path", "/"},
                                                                  {":authority", "www.example.com"},
                                                              },
                                                              {
                                                                  {":method", "GET"},
                                                                  {":scheme", "http"},
                                                                  {":path", "/"},
                                                                  {":authority", "www.example.com"},
                                                                  {"cache-control", "no-cache"},
                                                              },
                                                              {
                                                                  {":method", "GET"},
                                                                  {":scheme", "https"},
                                                                  {":path", "/index.html"},
                                                                  {":authority", "www.example.com"},
                                                                  {"custom-key", "custom-value"},
                                                              }};

    for (auto& example : examples) {
      auto data = hpack.encode(example);
      std::vector<chttp2::HeaderField> headers;
      REQUIRE(hpack.decode(data, headers));
      std::sort(headers.begin(), headers.end());
      std::sort(example.begin(), example.end());
      REQUIRE(headers == example);
    }
  }

  SECTION("dynamic table") {
    chttp2::HpackTable table;
    table.insert({"foo", "bar"});
    table.insert({"blake", "miz"});
    table.insert({":method", "GET"});

    REQUIRE(table.dynamicTableCnt() == 3);
    std::uint64_t idx = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(table.find({"foo", "bar"}, idx));
    REQUIRE(idx == 64);
    REQUIRE(table.find({"blake", "miz"}, idx));
    REQUIRE(idx == 63);
    REQUIRE(table.find({":method", "GET"}, idx));
    REQUIRE(idx == 2);

    REQUIRE_FALSE(table.find({":method", "GET", true}, idx));
    REQUIRE(idx == 2);

    REQUIRE_FALSE(table.find({"foo", "..."}, idx));
    REQUIRE(idx == 64);

    REQUIRE_FALSE(table.find({"blake", "..."}, idx));
    REQUIRE(idx == 63);

    REQUIRE_FALSE(table.find({":method", "..."}, idx));
    REQUIRE(idx == 2);

    REQUIRE_FALSE(table.find({"foo-", "bar"}, idx));
    REQUIRE(idx == 0);
  }

  SECTION("never-indexed encode") {
    auto buffer = hpack.encode({{"custom-key", "custom-value", true}});
    auto expected =
        chttp2::ByteBuffer::from(hexToBytes("10 88 25a8 49e9 5ba9 7d7f 89 25a8 49e9 5bb8 e8b4 bf"));
    REQUIRE(buffer == expected);
  }

  SECTION("table size update emits one update") {
    hpack.setEncodeTableSize(16384);
    hpack.setEncodeTableSize(2048);

    auto buffer = hpack.encode({{":method", "GET"}});
    REQUIRE(buffer == chttp2::ByteBuffer::from(hexToBytes("3fe1 0f82")));
  }

  SECTION("table size update emits two updates") {
    hpack.setEncodeTableSize(2048);
    hpack.setEncodeTableSize(4096);

    auto buffer = hpack.encode({{":method", "GET"}});
    REQUIRE(buffer == chttp2::ByteBuffer::from(hexToBytes("3fe10f 3fe11f 82")));
  }

  SECTION("name-only match from dynamic table") {
    chttp2::Hpack enc;
    chttp2::Hpack dec;
    // Insert {"x-id", "aaa"} into dynamic table
    auto d1 = enc.encode({{"x-id", "aaa"}});
    std::vector<chttp2::HeaderField> r1;
    REQUIRE(dec.decode(d1, r1));
    REQUIRE(r1[0] == chttp2::HeaderField{"x-id", "aaa"});

    // Name "x-id" matches dynamic table, value "bbb" is new
    auto d2 = enc.encode({{"x-id", "bbb"}});
    std::vector<chttp2::HeaderField> r2;
    REQUIRE(dec.decode(d2, r2));
    REQUIRE(r2[0] == chttp2::HeaderField{"x-id", "bbb"});
  }

  SECTION("eviction via table size change") {
    chttp2::Hpack enc;
    chttp2::Hpack dec;
    auto d1 = enc.encode({{"x-a", "111"}, {"x-b", "222"}});
    std::vector<chttp2::HeaderField> r1;
    REQUIRE(dec.decode(d1, r1));
    REQUIRE(r1.size() == 2);

    // Evict all, re-grow, re-encode
    enc.setEncodeTableSize(0);
    enc.setEncodeTableSize(4096);
    auto d2 = enc.encode({{"x-a", "111"}});
    std::vector<chttp2::HeaderField> r2;
    REQUIRE(dec.decode(d2, r2));
    REQUIRE(r2[0] == chttp2::HeaderField{"x-a", "111"});
  }

  SECTION("literal without indexing for oversized header") {
    chttp2::Hpack enc;
    chttp2::Hpack dec;
    enc.setEncodeTableSize(32);  // min entry is 33 bytes, nothing fits
    auto d1 = enc.encode({{"x", "y"}});
    std::vector<chttp2::HeaderField> r1;
    REQUIRE(dec.decode(d1, r1));
    REQUIRE(r1[0] == chttp2::HeaderField{"x", "y"});
  }
}
