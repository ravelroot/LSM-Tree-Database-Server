#pragma once

#include <cstdint>
#include <string_view>

#include "arca/write_batch.hpp"

namespace arca {

class MemTable;

class WriteBatchInternal {
 public:
  static constexpr size_t kHeader = 12;
  static uint32_t Count(const WriteBatch* batch);
  static void SetCount(WriteBatch* batch, uint32_t n);
  static uint64_t Sequence(const WriteBatch* batch);
  static void SetSequence(WriteBatch* batch, uint64_t seq);
  static std::string_view Contents(const WriteBatch* batch) { return batch->rep_; }
  static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }
  static void SetContents(WriteBatch* batch, std::string_view contents);
  static Status InsertInto(const WriteBatch* batch, MemTable* memtable);
  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}
