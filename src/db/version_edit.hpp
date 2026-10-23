#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "arca/status.hpp"
#include "db/dbformat.hpp"

namespace arca {

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  uint64_t entries = 0;
  uint64_t deletions = 0;
  InternalKey smallest;
  InternalKey largest;
};

class VersionEdit {
 public:
  void Clear();

  void SetLogNumber(uint64_t n) {
    has_log_number_ = true;
    log_number_ = n;
  }
  void SetNextFile(uint64_t n) {
    has_next_file_number_ = true;
    next_file_number_ = n;
  }
  void SetLastSequence(uint64_t s) {
    has_last_sequence_ = true;
    last_sequence_ = s;
  }
  void SetCompactPointer(int level, const InternalKey& key) { compact_pointers_.emplace_back(level, key); }
  void AddFile(int level, const FileMetaData& f) { new_files_.emplace_back(level, f); }
  void RemoveFile(int level, uint64_t number) { deleted_files_.insert({level, number}); }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);
  std::string DebugString() const;

 private:
  friend class VersionSet;

  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 0;
  uint64_t last_sequence_ = 0;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;
  std::vector<std::pair<int, InternalKey>> compact_pointers_;
  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}
