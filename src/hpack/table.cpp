#include "chttp2/hpack/table.hpp"

#include "chttp2/header_field.hpp"
#include "chttp2/log.hpp"

namespace chttp2 {
const HpackTable::NameTable HpackTable::STATIC_NAME_TABLE = {
    // NOLINT(bugprone-throwing-static-initialization)
    {":authority", 1},
    {":method", 2},  // first occurrence (GET), not 3 (POST)
    {":path", 4},    // first occurrence (/), not 5 (/index.html)
    {":scheme", 6},  // first occurrence (http), not 7 (https)
    {":status", 8},  // first occurrence (200), not 14 (500)
    {"accept-charset", 15},
    {"accept-encoding", 16},
    {"accept-language", 17},
    {"accept-ranges", 18},
    {"accept", 19},
    {"access-control-allow-origin", 20},
    {"age", 21},
    {"allow", 22},
    {"authorization", 23},
    {"cache-control", 24},
    {"content-disposition", 25},
    {"content-encoding", 26},
    {"content-language", 27},
    {"content-length", 28},
    {"content-location", 29},
    {"content-range", 30},
    {"content-type", 31},
    {"cookie", 32},
    {"date", 33},
    {"etag", 34},
    {"expect", 35},
    {"expires", 36},
    {"from", 37},
    {"host", 38},
    {"if-match", 39},
    {"if-modified-since", 40},
    {"if-none-match", 41},
    {"if-range", 42},
    {"if-unmodified-since", 43},
    {"last-modified", 44},
    {"link", 45},
    {"location", 46},
    {"max-forwards", 47},
    {"proxy-authenticate", 48},
    {"proxy-authorization", 49},
    {"range", 50},
    {"referer", 51},
    {"refresh", 52},
    {"retry-after", 53},
    {"server", 54},
    {"set-cookie", 55},
    {"strict-transport-security", 56},
    {"transfer-encoding", 57},
    {"user-agent", 58},
    {"vary", 59},
    {"via", 60},
    {"www-authenticate", 61},
};

const HpackTable::NameValueTable HpackTable::STATIC_NAME_VALUE_TABLE = {
    // NOLINT(bugprone-throwing-static-initialization)
    {{":authority", ""}, 1},
    {{":method", "GET"}, 2},
    {{":method", "POST"}, 3},
    {{":path", "/"}, 4},
    {{":path", "/index.html"}, 5},
    {{":scheme", "http"}, 6},
    {{":scheme", "https"}, 7},
    {{":status", "200"}, 8},
    {{":status", "204"}, 9},
    {{":status", "206"}, 10},
    {{":status", "304"}, 11},
    {{":status", "400"}, 12},
    {{":status", "404"}, 13},
    {{":status", "500"}, 14},
    {{"accept-charset", ""}, 15},
    {{"accept-encoding", "gzip, deflate"}, 16},
    {{"accept-language", ""}, 17},
    {{"accept-ranges", ""}, 18},
    {{"accept", ""}, 19},
    {{"access-control-allow-origin", ""}, 20},
    {{"age", ""}, 21},
    {{"allow", ""}, 22},
    {{"authorization", ""}, 23},
    {{"cache-control", ""}, 24},
    {{"content-disposition", ""}, 25},
    {{"content-encoding", ""}, 26},
    {{"content-language", ""}, 27},
    {{"content-length", ""}, 28},
    {{"content-location", ""}, 29},
    {{"content-range", ""}, 30},
    {{"content-type", ""}, 31},
    {{"cookie", ""}, 32},
    {{"date", ""}, 33},
    {{"etag", ""}, 34},
    {{"expect", ""}, 35},
    {{"expires", ""}, 36},
    {{"from", ""}, 37},
    {{"host", ""}, 38},
    {{"if-match", ""}, 39},
    {{"if-modified-since", ""}, 40},
    {{"if-none-match", ""}, 41},
    {{"if-range", ""}, 42},
    {{"if-unmodified-since", ""}, 43},
    {{"last-modified", ""}, 44},
    {{"link", ""}, 45},
    {{"location", ""}, 46},
    {{"max-forwards", ""}, 47},
    {{"proxy-authenticate", ""}, 48},
    {{"proxy-authorization", ""}, 49},
    {{"range", ""}, 50},
    {{"referer", ""}, 51},
    {{"refresh", ""}, 52},
    {{"retry-after", ""}, 53},
    {{"server", ""}, 54},
    {{"set-cookie", ""}, 55},
    {{"strict-transport-security", ""}, 56},
    {{"transfer-encoding", ""}, 57},
    {{"user-agent", ""}, 58},
    {{"vary", ""}, 59},
    {{"via", ""}, 60},
    {{"www-authenticate", ""}, 61},
};

const HpackTable::EntryTable HpackTable::STATIC_ENTRY_TABLE = {
    // NOLINT(bugprone-throwing-static-initialization)
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

bool HpackTable::find(const HeaderField& header, std::uint64_t& index) const {
  index = 0;

  if (!header.sensitivity) {
    // 1. check static name-value table
    auto it = STATIC_NAME_VALUE_TABLE.find({header.name, header.value});
    if (it != STATIC_NAME_VALUE_TABLE.end()) {
      index = it->second;
      return true;
    }
    // 2. check dynamic name-value table (skip if entry was evicted)
    auto it2 = nameValueTable.find({header.name, header.value});
    if (it2 != nameValueTable.end() && id2Index(it2->second, index)) {
      return true;
    }
  }

  // 3. check static name table
  auto it3 = STATIC_NAME_TABLE.find(header.name);
  if (it3 != STATIC_NAME_TABLE.end()) {
    index = it3->second;
    return false;
  }

  // 4. check dynamic name table (skip if entry was evicted)
  auto it4 = nameTable.find(header.name);
  if (it4 != nameTable.end() && id2Index(it4->second, index)) {
    return false;
  }

  return false;
}

bool HpackTable::id2Index(uint64_t id, uint64_t& index) const {
  auto oldestId = nextId - entryTable.size();
  if (id < oldestId) {
    return false;  // entry has been evicted
  }
  index = STATIC_ENTRY_TABLE.size() + (nextId - id);
  return true;
}

void HpackTable::insert(const HeaderField& header) {
  addEntry(header);
  dynamicBytes += header.size();
  evict();
}

void HpackTable::addEntry(const HeaderField& header) {
  nameTable[header.name] = nextId;
  nameValueTable[{header.name, header.value}] = nextId;
  entryTable.push_back(header);
  nextId++;
}

void HpackTable::evict() {
  auto oldestId = nextId - entryTable.size();
  std::size_t cnt = 0;
  while (dynamicBytes > maxSize && cnt < entryTable.size()) {
    auto& header = entryTable[cnt];
    auto id = oldestId + cnt;
    dynamicBytes -= header.size();

    auto nameIt = nameTable.find(header.name);
    if (nameIt != nameTable.end() && nameIt->second == id) {
      nameTable.erase(nameIt);
    }
    auto nvIt = nameValueTable.find({header.name, header.value});
    if (nvIt != nameValueTable.end() && nvIt->second == id) {
      nameValueTable.erase(nvIt);
    }
    cnt++;
  }
  entryTable.erase(entryTable.begin(), entryTable.begin() + static_cast<long>(cnt));
}

bool HpackTable::at(std::uint64_t index, HeaderField& out) const {
  // RFC 7541 Section 6.1: index 0 is not used
  if (index == 0) {
    return false;
  }

  if (index <= STATIC_ENTRY_TABLE.size()) {
    out = STATIC_ENTRY_TABLE[index - 1];
    return true;
  }

  auto dynamicIdx = index - STATIC_ENTRY_TABLE.size();
  if (dynamicIdx == 0 || dynamicIdx > entryTable.size()) {
    CHTTP2_LOG_WARN("[HPACK] invalid dynamic index: %lu, entryTable.size(): %lu",
                    dynamicIdx,
                    entryTable.size());
    return false;
  }

  out = entryTable[entryTable.size() - dynamicIdx];
  return true;
}

}  // namespace chttp2
