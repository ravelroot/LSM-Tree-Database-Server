#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "util/coding.hpp"

namespace arca {

inline constexpr int kNumLevels = 7;

enum ValueType : uint8_t { kTypeDeletion = 0, kTypeValue = 1 };

inline constexpr ValueType kValueTypeForSeek = kTypeValue;
inline constexpr uint64_t kMaxSequenceNumber = (1ull << 56) - 1;

inline uint64_t PackSequenceAndType(uint64_t seq, ValueType t) { return (seq << 8) | t; }

struct ParsedInternalKey {
  std::string_view user_key;
  uint64_t sequence = 0;
  ValueType type = kTypeValue;
};

inline void AppendInternalKey(std::string* result, std::string_view user_key, uint64_t seq, ValueType t) {
  result->append(user_key.data(), user_key.size());
  PutFixed64(result, PackSequenceAndType(seq, t));
}

inline bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result) {
  if (internal_key.size() < 8) return false;
  uint64_t num = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  uint8_t c = static_cast<uint8_t>(num & 0xff);
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = internal_key.substr(0, internal_key.size() - 8);
  return c <= static_cast<uint8_t>(kTypeValue);
}

inline std::string_view ExtractUserKey(std::string_view internal_key) {
  return internal_key.substr(0, internal_key.size() - 8);
}

class Comparator {
 public:
  virtual ~Comparator() = default;
  virtual int Compare(std::string_view a, std::string_view b) const = 0;
};

inline int CompareBytes(std::string_view a, std::string_view b) {
  int r = a.compare(b);
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

class InternalKeyComparator final : public Comparator {
 public:
  int Compare(std::string_view a, std::string_view b) const override {
    int r = CompareBytes(ExtractUserKey(a), ExtractUserKey(b));
    if (r != 0) return r;
    uint64_t anum = DecodeFixed64(a.data() + a.size() - 8);
    uint64_t bnum = DecodeFixed64(b.data() + b.size() - 8);
    if (anum > bnum) return -1;
    if (anum < bnum) return 1;
    return 0;
  }
};

class InternalKey {
 public:
  InternalKey() = default;
  InternalKey(std::string_view user_key, uint64_t seq, ValueType t) { AppendInternalKey(&rep_, user_key, seq, t); }

  void DecodeFrom(std::string_view s) { rep_.assign(s.data(), s.size()); }
  std::string_view Encode() const { return rep_; }
  std::string_view user_key() const { return ExtractUserKey(rep_); }
  bool empty() const { return rep_.empty(); }
  void Clear() { rep_.clear(); }

 private:
  std::string rep_;
};

class LookupKey {
 public:
  LookupKey(std::string_view user_key, uint64_t sequence) {
    PutVarint32(&buf_, static_cast<uint32_t>(user_key.size() + 8));
    kstart_ = buf_.size();
    AppendInternalKey(&buf_, user_key, sequence, kValueTypeForSeek);
  }

  std::string_view memtable_key() const { return buf_; }
  std::string_view internal_key() const { return std::string_view(buf_).substr(kstart_); }
  std::string_view user_key() const { return std::string_view(buf_).substr(kstart_, buf_.size() - kstart_ - 8); }

 private:
  std::string buf_;
  size_t kstart_ = 0;
};

struct EngineCounters {
  std::atomic<uint64_t> bloom_checks{0};
  std::atomic<uint64_t> bloom_useful{0};
  std::atomic<uint64_t> flushes{0};
  std::atomic<uint64_t> flush_bytes{0};
  std::atomic<uint64_t> compactions{0};
  std::atomic<uint64_t> compaction_bytes_read{0};
  std::atomic<uint64_t> compaction_bytes_written{0};
  std::atomic<uint64_t> user_bytes_written{0};
  std::atomic<uint64_t> write_stalls{0};
};

}
