#include "db/table_cache.hpp"

#include <mutex>

#include "db/filename.hpp"
#include "table/iterators.hpp"

namespace arca {

TableCache::TableCache(std::string dbname, const Options& options, BlockCache* block_cache, EngineCounters* counters)
    : dbname_(std::move(dbname)), options_(options), block_cache_(block_cache), counters_(counters) {}

Status TableCache::FindTable(uint64_t number, std::shared_ptr<Table>* table) {
  {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto it = tables_.find(number);
    if (it != tables_.end()) {
      it->second->last_used.store(tick_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
      *table = it->second->table;
      return Status::OK();
    }
  }
  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(TableFileName(dbname_, number), &file);
  if (!s.ok()) return s;
  std::shared_ptr<Table> opened;
  s = Table::Open(options_, std::move(file), block_cache_, counters_, &opened);
  if (!s.ok()) return s;
  std::unique_lock<std::shared_mutex> lock(mu_);
  auto it = tables_.find(number);
  if (it != tables_.end()) {
    *table = it->second->table;
    return Status::OK();
  }
  while (static_cast<int>(tables_.size()) >= options_.max_open_files && !tables_.empty()) EvictOneLocked();
  auto entry = std::make_unique<Entry>();
  entry->table = opened;
  entry->last_used.store(tick_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
  tables_.emplace(number, std::move(entry));
  *table = std::move(opened);
  return Status::OK();
}

void TableCache::EvictOneLocked() {
  auto victim = tables_.begin();
  for (auto it = tables_.begin(); it != tables_.end(); ++it) {
    if (it->second->last_used.load(std::memory_order_relaxed) < victim->second->last_used.load(std::memory_order_relaxed)) {
      victim = it;
    }
  }
  tables_.erase(victim);
}

std::unique_ptr<Iterator> TableCache::NewIterator(const ReadOptions& options, uint64_t number) {
  std::shared_ptr<Table> table;
  Status s = FindTable(number, &table);
  if (!s.ok()) return NewErrorIterator(s);
  return table->NewIterator(options);
}

Status TableCache::Get(const ReadOptions& options, uint64_t number, std::string_view internal_key,
                       const std::function<void(std::string_view, std::string_view)>& handle) {
  std::shared_ptr<Table> table;
  Status s = FindTable(number, &table);
  if (!s.ok()) return s;
  return table->InternalGet(options, internal_key, handle);
}

void TableCache::Evict(uint64_t number) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  tables_.erase(number);
}

size_t TableCache::OpenTables() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return tables_.size();
}

}
