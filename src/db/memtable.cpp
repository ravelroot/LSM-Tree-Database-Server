#include "db/memtable.hpp"

#include <cstring>

namespace arca {

namespace {

std::string_view GetLengthPrefixedSlice(const char* data) {
  uint32_t len = 0;
  const char* p = GetVarint32Ptr(data, data + 5, &len);
  return std::string_view(p, len);
}

}

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator.Compare(GetLengthPrefixedSlice(a), GetLengthPrefixedSlice(b));
}

MemTable::MemTable() : table_(KeyComparator{}, &arena_) {}

void MemTable::Add(uint64_t seq, ValueType type, std::string_view key, std::string_view value) {
  size_t internal_key_size = key.size() + 8;
  size_t encoded_len = static_cast<size_t>(VarintLength(internal_key_size)) + internal_key_size +
                       static_cast<size_t>(VarintLength(value.size())) + value.size();
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += 8;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  std::memcpy(p, value.data(), value.size());
  table_.Insert(buf);
  entries_.fetch_add(1, std::memory_order_relaxed);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  std::string_view memkey = key.memtable_key();
  Table::Iterator iter(&table_);
  iter.Seek(memkey.data());
  if (!iter.Valid()) return false;
  const char* entry = iter.key();
  uint32_t key_length = 0;
  const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
  std::string_view entry_user_key(key_ptr, key_length - 8);
  if (entry_user_key != key.user_key()) return false;
  uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
  switch (static_cast<ValueType>(tag & 0xff)) {
    case kTypeValue: {
      std::string_view v = GetLengthPrefixedSlice(key_ptr + key_length);
      value->assign(v.data(), v.size());
      *s = Status::OK();
      return true;
    }
    case kTypeDeletion:
      *s = Status::NotFound();
      return true;
  }
  return false;
}

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(std::shared_ptr<const MemTable> mem) : mem_(std::move(mem)), iter_(&mem_->table_) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Seek(std::string_view target) override {
    tmp_.clear();
    PutVarint32(&tmp_, static_cast<uint32_t>(target.size()));
    tmp_.append(target.data(), target.size());
    iter_.Seek(tmp_.data());
  }
  void Next() override { iter_.Next(); }
  std::string_view key() const override { return GetLengthPrefixedSlice(iter_.key()); }
  std::string_view value() const override {
    std::string_view k = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(k.data() + k.size());
  }
  Status status() const override { return Status::OK(); }

 private:
  std::shared_ptr<const MemTable> mem_;
  MemTable::Table::Iterator iter_;
  std::string tmp_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(shared_from_this());
}

}
