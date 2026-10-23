#include "table/cache.hpp"

namespace arca {

BlockCache::BlockCache(size_t capacity) : capacity_(capacity) {
  for (auto& s : shards_) s.capacity = capacity / kShards + 1;
}

std::shared_ptr<const Block> BlockCache::Lookup(uint64_t id, uint64_t offset) {
  Key k{id, offset};
  Shard& shard = ShardFor(k);
  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.map.find(k);
  if (it == shard.map.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
  hits_.fetch_add(1, std::memory_order_relaxed);
  return it->second->block;
}

void BlockCache::Insert(uint64_t id, uint64_t offset, std::shared_ptr<const Block> block) {
  if (capacity_ == 0) return;
  Key k{id, offset};
  size_t charge = block->size() + 64;
  Shard& shard = ShardFor(k);
  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.map.find(k);
  if (it != shard.map.end()) {
    shard.usage -= it->second->charge;
    shard.lru.erase(it->second);
    shard.map.erase(it);
  }
  shard.lru.push_front(Entry{k, std::move(block), charge});
  shard.map[k] = shard.lru.begin();
  shard.usage += charge;
  while (shard.usage > shard.capacity && shard.lru.size() > 1) {
    auto& last = shard.lru.back();
    shard.usage -= last.charge;
    shard.map.erase(last.key);
    shard.lru.pop_back();
  }
}

size_t BlockCache::Usage() const {
  size_t total = 0;
  for (const auto& s : shards_) {
    std::lock_guard<std::mutex> lock(s.mu);
    total += s.usage;
  }
  return total;
}

}
