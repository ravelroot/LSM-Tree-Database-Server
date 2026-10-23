#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "arca/status.hpp"
#include "util/file.hpp"

namespace arca::wal {

enum RecordType : uint8_t { kZeroType = 0, kFullType = 1, kFirstType = 2, kMiddleType = 3, kLastType = 4 };

inline constexpr int kMaxRecordType = kLastType;
inline constexpr size_t kBlockSize = 32768;
inline constexpr size_t kHeaderSize = 7;

class Writer {
 public:
  explicit Writer(WritableFile* dest, uint64_t dest_length = 0);
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  Status AddRecord(std::string_view record);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* data, size_t length);

  WritableFile* dest_;
  size_t block_offset_;
  uint32_t type_crc_[kMaxRecordType + 1];
};

class Reader {
 public:
  class Reporter {
   public:
    virtual ~Reporter() = default;
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  Reader(SequentialFile* file, Reporter* reporter);
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  bool ReadRecord(std::string_view* record, std::string* scratch);
  uint64_t LastRecordOffset() const { return last_record_offset_; }

 private:
  enum { kEof = kMaxRecordType + 1, kBadRecord = kMaxRecordType + 2 };

  unsigned int ReadPhysicalRecord(std::string_view* result);
  void ReportCorruption(size_t bytes, const char* reason);

  SequentialFile* file_;
  Reporter* reporter_;
  std::string backing_store_;
  std::string_view buffer_;
  bool eof_ = false;
  uint64_t last_record_offset_ = 0;
  uint64_t end_of_buffer_offset_ = 0;
};

}
