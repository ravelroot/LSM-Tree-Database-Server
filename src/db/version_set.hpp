#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "arca/iterator.hpp"
#include "arca/options.hpp"
#include "db/dbformat.hpp"
#include "db/log.hpp"
#include "db/table_cache.hpp"
#include "db/version_edit.hpp"

namespace arca {

using FileList = std::vector<std::shared_ptr<FileMetaData>>;

class Version {
 public:
  Version(TableCache* table_cache, const InternalKeyComparator* icmp) : table_cache_(table_cache), icmp_(icmp) {}

  Status Get(const ReadOptions& options, const LookupKey& key, std::string* value) const;
  void AddIterators(const ReadOptions& options, std::vector<std::unique_ptr<Iterator>>* iters) const;
  void GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, FileList* inputs) const;
  int NumFiles(int level) const { return static_cast<int>(files_[level].size()); }
  uint64_t LevelBytes(int level) const;

  FileList files_[kNumLevels];
  double compaction_score_ = -1;
  int compaction_level_ = -1;

 private:
  TableCache* table_cache_;
  const InternalKeyComparator* icmp_;
};

class Compaction {
 public:
  Compaction(const Options* options, int level);

  int level() const { return level_; }
  VersionEdit* edit() { return &edit_; }
  int num_input_files(int which) const { return static_cast<int>(inputs_[which].size()); }
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }
  bool IsTrivialMove() const;
  void AddInputDeletions(VersionEdit* edit);
  bool IsBaseLevelForKey(std::string_view user_key);
  bool ShouldStopBefore(std::string_view internal_key);

  std::shared_ptr<const Version> input_version_;
  FileList inputs_[2];
  FileList grandparents_;
  std::string reason_;

 private:
  int level_;
  uint64_t max_output_file_size_;
  VersionEdit edit_;
  size_t grandparent_index_ = 0;
  bool seen_key_ = false;
  int64_t overlapped_bytes_ = 0;
  size_t level_ptrs_[kNumLevels] = {};
  InternalKeyComparator icmp_;
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options* options, TableCache* table_cache, const InternalKeyComparator* icmp);
  ~VersionSet();

  Status LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>& lock);
  Status Recover();

  std::shared_ptr<const Version> current() const { return current_; }
  uint64_t NewFileNumber() { return next_file_number_++; }
  void ReuseFileNumber(uint64_t n) {
    if (next_file_number_ == n + 1) next_file_number_ = n;
  }
  void MarkFileNumberUsed(uint64_t n) {
    if (next_file_number_ <= n) next_file_number_ = n + 1;
  }
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t LogNumber() const { return log_number_; }
  uint64_t LastSequence() const { return last_sequence_.load(std::memory_order_acquire); }
  void SetLastSequence(uint64_t s) { last_sequence_.store(s, std::memory_order_release); }

  bool NeedsCompaction() const { return current_->compaction_score_ >= 1; }
  std::unique_ptr<Compaction> PickCompaction();
  std::unique_ptr<Compaction> CompactLevel(int level);
  std::unique_ptr<Iterator> MakeInputIterator(Compaction* c);
  void AddLiveFiles(std::set<uint64_t>* live);
  uint64_t MaxBytesForLevel(int level) const;
  void WriteSnapshotEdit(VersionEdit* edit) const;

 private:
  class Builder;

  void Finalize(Version* v) const;
  void AppendVersion(std::shared_ptr<Version> v);
  void SetupOtherInputs(Compaction* c);
  void GetRange(const FileList& inputs, InternalKey* smallest, InternalKey* largest) const;

  const std::string dbname_;
  const Options* options_;
  TableCache* table_cache_;
  const InternalKeyComparator* icmp_;
  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t log_number_ = 0;
  std::atomic<uint64_t> last_sequence_{0};
  std::unique_ptr<WritableFile> descriptor_file_;
  std::unique_ptr<wal::Writer> descriptor_log_;
  std::shared_ptr<Version> current_;
  std::vector<std::weak_ptr<const Version>> live_versions_;
  std::string compact_pointer_[kNumLevels];
};

}
