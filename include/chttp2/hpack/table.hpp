#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "chttp2/header_field.hpp"

namespace chttp2 {
constexpr std::size_t INIT_MAX_TABLE_SIZE = 4096;
struct PairHash {
  std::size_t operator()(const std::pair<std::string, std::string>& pair) const {
    std::size_t seed = std::hash<std::string>()(pair.first);
    seed ^=
        std::hash<std::string>()(pair.second) + 0x9e3779b97f4a7c15ULL + (seed << 12) + (seed >> 4);
    return seed;
  }
};

class HpackTable {
 public:
  HpackTable() = default;
  ~HpackTable() = default;

  // Returns true if exact match (name+value), false otherwise.
  // On name-only match, index is set to the matching entry; on no match, index is 0.
  bool find(const HeaderField& header, std::uint64_t& index) const;

  // insert a new header field into the dynamic table
  void insert(const HeaderField& header);

  // Look up a header by HPACK index (1-based, spanning static + dynamic table).
  // Returns false on invalid index (0, out-of-range) — caller should treat as COMPRESSION_ERROR.
  bool at(std::uint64_t index, HeaderField& out) const;

  std::size_t dynamicTableCnt() const { return entryTable.size(); }
  std::uint64_t dynamicTableBytes() const { return dynamicBytes; }

  std::size_t getMaxSize() const { return maxSize; }

  void setMaxSize(std::size_t size) {
    maxSize = size;
    evict();
  }

 private:
  // only used when find a header field in the dynamic table
  // https://httpwg.org/specs/rfc7541.html#index.address.space
  //     <----------  Index Address Space ---------->
  //     <-- Static  Table -->  <-- Dynamic Table -->
  //     +---+-----------+---+  +---+-----------+---+
  //     | 1 |    ...    | s |  |s+1|    ...    |s+k|
  //     +---+-----------+---+  +---+-----------+---+
  //                            ^                   |
  //                            |                   V
  //                     Insertion Point      Dropping Point
  // Convert internal monotonic ID to HPACK index. Returns false if the entry
  // has been evicted (stale reference in name/nameValue lookup tables).
  bool id2Index(uint64_t id, uint64_t& index) const;

  void addEntry(const HeaderField& header);

  void evict();

  using NameTable = std::unordered_map<std::string, std::uint64_t>;
  using EntryTable = std::vector<HeaderField>;
  using NameValueTable =
      std::unordered_map<std::pair<std::string, std::string>, std::uint64_t, PairHash>;

  // Static table (RFC 7541 Appendix A) — same 61 entries in three forms
  // for O(1) lookup in different directions:
  const static EntryTable STATIC_ENTRY_TABLE;  // Decode: index → HeaderField
  const static NameTable STATIC_NAME_TABLE;    // Encode: name → index (name-only match)
  const static NameValueTable
      STATIC_NAME_VALUE_TABLE;  // Encode: (name,value) → index (exact match)

  // dynamic table
  NameTable nameTable;
  NameValueTable nameValueTable;
  EntryTable entryTable;

  std::uint64_t maxSize{INIT_MAX_TABLE_SIZE};
  std::uint64_t nextId{1};  // next monotonic ID to assign (1-based)
  std::uint64_t dynamicBytes{0};
};
}  // namespace chttp2
