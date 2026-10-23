#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "table/block.hpp"

namespace arca {

class BlockCache {
 public:
  explicit BlockCache(size_t capacity);

  std::shared_ptr<const Block> Lookup(uint64_t id, uint64_t offset);
  void Insert(uint64_t id, uint64_t offset, std::shared_ptr<const Block> block);
  uint64_t NewId() { return next_id_.fetch_add(1, std::memory_order_relaxed); }

  size_t Usage() const;
  size_t Capacity() const { return capacity_; }
  uint64_t Hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t Misses() const { return misses_.load(std::memory_order_relaxed); }

 private:
  struct Key {
    uint64_t id;
    uint64_t offset;
    bool operator==(const Key& o) const { return id == o.id && offset == o.offset; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const { return std::hash<uint64_t>()(k.id * 0x9e3779b97f4a7c15ull ^ k.offset); }
  };
  struct Entry {
    Key key;
    std::shared_ptr<const Block> block;
    size_t charge;
  };
  struct Shard {
    mutable std::mutex mu;
    std::list<Entry> lru;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map;
    size_t usage = 0;
    size_t capacity = 0;
  };

  static constexpr size_t kShards = 16;
  Shard& ShardFor(const Key& k) { return shards_[KeyHash()(k) % kShards]; }

  size_t capacity_;
  std::array<Shard, kShards> shards_;
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
  std::atomic<uint64_t> next_id_{1};
};

}
