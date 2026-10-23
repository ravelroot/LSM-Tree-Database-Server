#pragma once

#include <atomic>
#include <cstdint>
#include <new>

#include "util/arena.hpp"

namespace arca {

template <typename Key, class KeyComparator>
class SkipList {
 private:
  struct Node;

 public:
  SkipList(KeyComparator cmp, Arena* arena)
      : compare_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)), max_height_(1), rnd_(0xdeadbeef) {
    for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    FindGreaterOrEqual(key, prev);
    int height = RandomHeight();
    if (height > GetMaxHeight()) {
      for (int i = GetMaxHeight(); i < height; ++i) prev[i] = head_;
      max_height_.store(height, std::memory_order_relaxed);
    }
    Node* x = NewNode(key, height);
    for (int i = 0; i < height; ++i) {
      x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
      prev[i]->SetNext(i, x);
    }
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const { return node_->key; }
    void Next() { node_ = node_->Next(0); }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kMaxHeight = 12;

  struct Node {
    explicit Node(const Key& k) : key(k) {}
    Key const key;
    Node* Next(int n) { return next_[n].load(std::memory_order_acquire); }
    void SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_release); }
    Node* NoBarrierNext(int n) { return next_[n].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }

   private:
    std::atomic<Node*> next_[1];
  };

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  Node* NewNode(const Key& key, int height) {
    char* mem = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height - 1));
    return new (mem) Node(key);
  }

  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && NextRandom() % 4 == 0) ++height;
    return height;
  }

  uint32_t NextRandom() {
    rnd_ ^= rnd_ << 13;
    rnd_ ^= rnd_ >> 17;
    rnd_ ^= rnd_ << 5;
    return rnd_;
  }

  bool KeyIsAfterNode(const Key& key, Node* n) const { return n != nullptr && compare_(n->key, key) < 0; }

  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (KeyIsAfterNode(key, next)) {
        x = next;
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        --level;
      }
    }
  }

  KeyComparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
  uint32_t rnd_;
};

}
