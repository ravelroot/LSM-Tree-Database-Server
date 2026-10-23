#include "table/block.hpp"

#include <algorithm>

#include "table/iterators.hpp"
#include "util/coding.hpp"

namespace arca {

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) { Reset(); }

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  size_t shared = 0;
  if (counter_ < restart_interval_) {
    size_t min_length = std::min(last_key_.size(), key.size());
    while (shared < min_length && last_key_[shared] == key[shared]) ++shared;
  } else {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  ++counter_;
}

std::string_view BlockBuilder::Finish() {
  for (uint32_t r : restarts_) PutFixed32(&buffer_, r);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

Block::Block(std::string contents) : data_(std::move(contents)) {
  if (data_.size() < sizeof(uint32_t)) {
    corrupt_ = true;
    return;
  }
  size_t max_restarts = (data_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  num_restarts_ = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
  if (num_restarts_ == 0 || num_restarts_ > max_restarts) {
    corrupt_ = true;
    return;
  }
  restart_offset_ = static_cast<uint32_t>(data_.size() - (1 + num_restarts_) * sizeof(uint32_t));
}

namespace {

const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared, uint32_t* non_shared, uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
  if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
  if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) return nullptr;
  return p;
}

class BlockIterator final : public Iterator {
 public:
  BlockIterator(std::shared_ptr<const Block> block, const Comparator* cmp, const char* data, uint32_t restarts,
                uint32_t num_restarts)
      : block_(std::move(block)),
        comparator_(cmp),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts),
        restart_index_(num_restarts) {}

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }

  void Next() override { ParseNextKey(); }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void Seek(std::string_view target) override {
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      uint32_t mid = (left + right + 1) / 2;
      uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared = 0;
      uint32_t non_shared = 0;
      uint32_t value_length = 0;
      const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_, &shared, &non_shared, &value_length);
      if (key_ptr == nullptr || shared != 0) {
        CorruptionError();
        return;
      }
      std::string_view mid_key(key_ptr, non_shared);
      if (comparator_->Compare(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }
    SeekToRestartPoint(left);
    while (true) {
      if (!ParseNextKey()) return;
      if (comparator_->Compare(key_, target) >= 0) return;
    }
  }

 private:
  uint32_t NextEntryOffset() const { return static_cast<uint32_t>((value_.data() + value_.size()) - data_); }

  uint32_t GetRestartPoint(uint32_t index) const { return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t)); }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    uint32_t offset = GetRestartPoint(index);
    value_ = std::string_view(data_ + offset, 0);
  }

  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bloco com entradas inválidas");
    key_.clear();
    value_ = {};
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;
    if (p >= limit) {
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = std::string_view(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ && GetRestartPoint(restart_index_ + 1) < current_) ++restart_index_;
    return true;
  }

  std::shared_ptr<const Block> block_;
  const Comparator* comparator_;
  const char* data_;
  uint32_t restarts_;
  uint32_t num_restarts_;
  uint32_t current_;
  uint32_t restart_index_;
  std::string key_;
  std::string_view value_;
  Status status_;
};

}

std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block, const Comparator* cmp) {
  if (block->corrupt_) return NewErrorIterator(Status::Corruption("bloco corrompido"));
  const char* data = block->data_.data();
  uint32_t restarts = block->restart_offset_;
  uint32_t num_restarts = block->num_restarts_;
  return std::make_unique<BlockIterator>(std::move(block), cmp, data, restarts, num_restarts);
}

}
