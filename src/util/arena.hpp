#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace arca {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes) {
    if (bytes <= remaining_) {
      char* result = ptr_;
      ptr_ += bytes;
      remaining_ -= bytes;
      return result;
    }
    return AllocateFallback(bytes);
  }

  char* AllocateAligned(size_t bytes) {
    constexpr size_t align = alignof(std::max_align_t);
    size_t mod = reinterpret_cast<uintptr_t>(ptr_) & (align - 1);
    size_t slop = mod == 0 ? 0 : align - mod;
    size_t needed = bytes + slop;
    if (needed <= remaining_) {
      char* result = ptr_ + slop;
      ptr_ += needed;
      remaining_ -= needed;
      return result;
    }
    return AllocateFallback(bytes);
  }

  size_t MemoryUsage() const { return usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) return AllocateNewBlock(bytes);
    ptr_ = AllocateNewBlock(kBlockSize);
    remaining_ = kBlockSize;
    char* result = ptr_;
    ptr_ += bytes;
    remaining_ -= bytes;
    return result;
  }

  char* AllocateNewBlock(size_t bytes) {
    blocks_.push_back(std::make_unique<char[]>(bytes));
    usage_.fetch_add(bytes + sizeof(char*), std::memory_order_relaxed);
    return blocks_.back().get();
  }

  char* ptr_ = nullptr;
  size_t remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> usage_{0};
};

}
