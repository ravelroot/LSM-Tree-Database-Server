#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "arca/iterator.hpp"
#include "arca/status.hpp"
#include "db/dbformat.hpp"
#include "db/skiplist.hpp"
#include "util/arena.hpp"

namespace arca {

class MemTable : public std::enable_shared_from_this<MemTable> {
 public:
  MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }
  uint64_t Entries() const { return entries_.load(std::memory_order_relaxed); }

  void Add(uint64_t seq, ValueType type, std::string_view key, std::string_view value);
  bool Get(const LookupKey& key, std::string* value, Status* s) const;
  std::unique_ptr<Iterator> NewIterator() const;

 private:
  struct KeyComparator {
    InternalKeyComparator comparator;
    int operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

  friend class MemTableIterator;

  Arena arena_;
  Table table_;
  std::atomic<uint64_t> entries_{0};
};

}
