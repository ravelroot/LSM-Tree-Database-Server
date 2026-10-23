#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "arca/iterator.hpp"
#include "arca/options.hpp"
#include "table/cache.hpp"
#include "table/table.hpp"

namespace arca {

class TableCache {
 public:
  TableCache(std::string dbname, const Options& options, BlockCache* block_cache, EngineCounters* counters);

  Status FindTable(uint64_t number, std::shared_ptr<Table>* table);
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options, uint64_t number);
  Status Get(const ReadOptions& options, uint64_t number, std::string_view internal_key,
             const std::function<void(std::string_view, std::string_view)>& handle);
  void Evict(uint64_t number);
  size_t OpenTables() const;

 private:
  struct Entry {
    std::shared_ptr<Table> table;
    std::atomic<uint64_t> last_used{0};
  };

  void EvictOneLocked();

  const std::string dbname_;
  const Options& options_;
  BlockCache* block_cache_;
  EngineCounters* counters_;
  mutable std::shared_mutex mu_;
  std::unordered_map<uint64_t, std::unique_ptr<Entry>> tables_;
  std::atomic<uint64_t> tick_{0};
};

}
