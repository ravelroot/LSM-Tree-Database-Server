#include "db/version_set.hpp"

#include <algorithm>
#include <cstdio>

#include "db/filename.hpp"
#include "table/iterators.hpp"
#include "util/coding.hpp"
#include "util/file.hpp"

namespace arca {

namespace {

uint64_t TotalFileSize(const FileList& files) {
  uint64_t sum = 0;
  for (const auto& f : files) sum += f->file_size;
  return sum;
}

size_t FindFile(const InternalKeyComparator& icmp, const FileList& files, std::string_view key) {
  size_t left = 0;
  size_t right = files.size();
  while (left < right) {
    size_t mid = (left + right) / 2;
    if (icmp.Compare(files[mid]->largest.Encode(), key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return right;
}

class LevelFileNumIterator final : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator* icmp, FileList files)
      : icmp_(icmp), files_(std::move(files)), index_(files_.size()) {}

  bool Valid() const override { return index_ < files_.size(); }
  void Seek(std::string_view target) override { index_ = FindFile(*icmp_, files_, target); }
  void SeekToFirst() override { index_ = 0; }
  void Next() override { ++index_; }
  std::string_view key() const override { return files_[index_]->largest.Encode(); }
  std::string_view value() const override {
    EncodeFixed64(value_buf_, files_[index_]->number);
    return std::string_view(value_buf_, 8);
  }
  Status status() const override { return Status::OK(); }

 private:
  const InternalKeyComparator* icmp_;
  FileList files_;
  size_t index_;
  mutable char value_buf_[8];
};

std::unique_ptr<Iterator> NewConcatenatingIterator(const InternalKeyComparator* icmp, TableCache* cache,
                                                   const FileList& files, const ReadOptions& options) {
  return NewTwoLevelIterator(std::make_unique<LevelFileNumIterator>(icmp, files),
                             [cache, options](std::string_view value) {
                               if (value.size() != 8) return NewErrorIterator(Status::Corruption("referência de arquivo inválida"));
                               return cache->NewIterator(options, DecodeFixed64(value.data()));
                             });
}

class CorruptionReporter final : public wal::Reader::Reporter {
 public:
  explicit CorruptionReporter(Status* status) : status_(status) {}
  void Corruption(size_t, const Status& s) override {
    if (status_->ok()) *status_ = s;
  }

 private:
  Status* status_;
};

}

uint64_t Version::LevelBytes(int level) const { return TotalFileSize(files_[level]); }

Status Version::Get(const ReadOptions& options, const LookupKey& k, std::string* value) const {
  std::string_view user_key = k.user_key();
  std::string_view ikey = k.internal_key();
  enum class State { kNotFound, kFound, kDeleted, kCorrupt };

  auto search = [&](const FileMetaData& f, State* state) -> Status {
    *state = State::kNotFound;
    return table_cache_->Get(options, f.number, ikey, [&](std::string_view found_key, std::string_view found_value) {
      ParsedInternalKey parsed;
      if (!ParseInternalKey(found_key, &parsed)) {
        *state = State::kCorrupt;
        return;
      }
      if (parsed.user_key != user_key) return;
      if (parsed.type == kTypeValue) {
        *state = State::kFound;
        value->assign(found_value.data(), found_value.size());
      } else {
        *state = State::kDeleted;
      }
    });
  };

  std::vector<const FileMetaData*> level0;
  for (const auto& f : files_[0]) {
    if (user_key >= f->smallest.user_key() && user_key <= f->largest.user_key()) level0.push_back(f.get());
  }
  std::sort(level0.begin(), level0.end(), [](const FileMetaData* a, const FileMetaData* b) { return a->number > b->number; });

  auto handle = [&](const FileMetaData& f, bool* done) -> Status {
    State state;
    Status s = search(f, &state);
    if (!s.ok()) {
      *done = true;
      return s;
    }
    switch (state) {
      case State::kNotFound:
        *done = false;
        return Status::OK();
      case State::kFound:
        *done = true;
        return Status::OK();
      case State::kDeleted:
        *done = true;
        return Status::NotFound();
      case State::kCorrupt:
        *done = true;
        return Status::Corruption("chave interna inválida na SSTable " + std::to_string(f.number));
    }
    return Status::OK();
  };

  for (const FileMetaData* f : level0) {
    bool done = false;
    Status s = handle(*f, &done);
    if (done) return s;
  }
  for (int level = 1; level < kNumLevels; ++level) {
    const FileList& files = files_[level];
    if (files.empty()) continue;
    size_t index = FindFile(*icmp_, files, ikey);
    if (index >= files.size()) continue;
    const FileMetaData& f = *files[index];
    if (user_key < f.smallest.user_key()) continue;
    bool done = false;
    Status s = handle(f, &done);
    if (done) return s;
  }
  return Status::NotFound();
}

void Version::AddIterators(const ReadOptions& options, std::vector<std::unique_ptr<Iterator>>* iters) const {
  for (const auto& f : files_[0]) iters->push_back(table_cache_->NewIterator(options, f->number));
  for (int level = 1; level < kNumLevels; ++level) {
    if (!files_[level].empty()) iters->push_back(NewConcatenatingIterator(icmp_, table_cache_, files_[level], options));
  }
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, FileList* inputs) const {
  inputs->clear();
  std::string user_begin = begin != nullptr ? std::string(begin->user_key()) : std::string();
  std::string user_end = end != nullptr ? std::string(end->user_key()) : std::string();
  for (size_t i = 0; i < files_[level].size();) {
    const auto& f = files_[level][i++];
    std::string_view file_start = f->smallest.user_key();
    std::string_view file_limit = f->largest.user_key();
    if (begin != nullptr && file_limit < user_begin) continue;
    if (end != nullptr && file_start > user_end) continue;
    inputs->push_back(f);
    if (level == 0) {
      if (begin != nullptr && file_start < user_begin) {
        user_begin.assign(file_start.data(), file_start.size());
        inputs->clear();
        i = 0;
      } else if (end != nullptr && file_limit > user_end) {
        user_end.assign(file_limit.data(), file_limit.size());
        inputs->clear();
        i = 0;
      }
    }
  }
}

Compaction::Compaction(const Options* options, int level) : level_(level), max_output_file_size_(options->max_file_size) {}

bool Compaction::IsTrivialMove() const {
  return inputs_[0].size() == 1 && inputs_[1].empty() && TotalFileSize(grandparents_) <= 10 * max_output_file_size_;
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : inputs_[which]) edit->RemoveFile(level_ + which, f->number);
  }
}

bool Compaction::IsBaseLevelForKey(std::string_view user_key) {
  for (int lvl = level_ + 2; lvl < kNumLevels; ++lvl) {
    const FileList& files = input_version_->files_[lvl];
    while (level_ptrs_[lvl] < files.size()) {
      const auto& f = files[level_ptrs_[lvl]];
      if (user_key <= f->largest.user_key()) {
        if (user_key >= f->smallest.user_key()) return false;
        break;
      }
      ++level_ptrs_[lvl];
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(std::string_view internal_key) {
  while (grandparent_index_ < grandparents_.size() &&
         icmp_.Compare(internal_key, grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    if (seen_key_) overlapped_bytes_ += static_cast<int64_t>(grandparents_[grandparent_index_]->file_size);
    ++grandparent_index_;
  }
  seen_key_ = true;
  if (overlapped_bytes_ > static_cast<int64_t>(10 * max_output_file_size_)) {
    overlapped_bytes_ = 0;
    return true;
  }
  return false;
}

class VersionSet::Builder {
 public:
  Builder(const Version* base) : base_(base) {}

  void Apply(const VersionEdit* edit, std::string* compact_pointers) {
    for (const auto& [level, key] : edit->compact_pointers_) compact_pointers[level] = std::string(key.Encode());
    for (const auto& [level, number] : edit->deleted_files_) deleted_[level].insert(number);
    for (const auto& [level, f] : edit->new_files_) {
      deleted_[level].erase(f.number);
      added_[level].push_back(std::make_shared<FileMetaData>(f));
    }
  }

  void SaveTo(Version* v, const InternalKeyComparator& icmp) {
    for (int level = 0; level < kNumLevels; ++level) {
      FileList merged;
      for (const auto& f : base_->files_[level]) {
        if (deleted_[level].count(f->number) == 0) merged.push_back(f);
      }
      for (const auto& f : added_[level]) {
        if (deleted_[level].count(f->number) == 0) merged.push_back(f);
      }
      if (level == 0) {
        std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) { return a->number < b->number; });
      } else {
        std::sort(merged.begin(), merged.end(), [&icmp](const auto& a, const auto& b) {
          return icmp.Compare(a->smallest.Encode(), b->smallest.Encode()) < 0;
        });
      }
      v->files_[level] = std::move(merged);
    }
  }

 private:
  const Version* base_;
  std::set<uint64_t> deleted_[kNumLevels];
  FileList added_[kNumLevels];
};

VersionSet::VersionSet(std::string dbname, const Options* options, TableCache* table_cache,
                       const InternalKeyComparator* icmp)
    : dbname_(std::move(dbname)), options_(options), table_cache_(table_cache), icmp_(icmp) {
  AppendVersion(std::make_shared<Version>(table_cache_, icmp_));
}

VersionSet::~VersionSet() {
  descriptor_log_.reset();
  if (descriptor_file_) descriptor_file_->Close();
}

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  if (level <= 0) return 0;
  double result = static_cast<double>(options_->level1_max_bytes);
  for (int i = 1; i < level; ++i) result *= options_->level_size_multiplier;
  return static_cast<uint64_t>(result);
}

void VersionSet::Finalize(Version* v) const {
  int best_level = -1;
  double best_score = -1;
  for (int level = 0; level < kNumLevels - 1; ++level) {
    double score;
    if (level == 0) {
      score = static_cast<double>(v->files_[0].size()) / static_cast<double>(options_->level0_compaction_trigger);
    } else {
      score = static_cast<double>(v->LevelBytes(level)) / static_cast<double>(MaxBytesForLevel(level));
    }
    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

void VersionSet::AppendVersion(std::shared_ptr<Version> v) {
  current_ = v;
  live_versions_.erase(std::remove_if(live_versions_.begin(), live_versions_.end(),
                                      [](const std::weak_ptr<const Version>& w) { return w.expired(); }),
                       live_versions_.end());
  live_versions_.push_back(v);
}

void VersionSet::WriteSnapshotEdit(VersionEdit* edit) const {
  for (int level = 0; level < kNumLevels; ++level) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit->SetCompactPointer(level, key);
    }
    for (const auto& f : current_->files_[level]) edit->AddFile(level, *f);
  }
}

Status VersionSet::LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>& lock) {
  if (!edit->has_log_number_) edit->SetLogNumber(log_number_);
  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(LastSequence());

  auto v = std::make_shared<Version>(table_cache_, icmp_);
  {
    Builder builder(current_.get());
    builder.Apply(edit, compact_pointer_);
    builder.SaveTo(v.get(), *icmp_);
  }
  Finalize(v.get());

  std::string new_manifest;
  Status s;
  if (!descriptor_log_) {
    manifest_file_number_ = NewFileNumber();
    edit->SetNextFile(next_file_number_);
    new_manifest = ManifestFileName(dbname_, manifest_file_number_);
    s = WritableFile::Open(new_manifest, true, &descriptor_file_);
    if (s.ok()) {
      descriptor_log_ = std::make_unique<wal::Writer>(descriptor_file_.get());
      VersionEdit snapshot;
      WriteSnapshotEdit(&snapshot);
      std::string record;
      snapshot.EncodeTo(&record);
      s = descriptor_log_->AddRecord(record);
    }
  }

  lock.unlock();
  if (s.ok()) {
    std::string record;
    edit->EncodeTo(&record);
    s = descriptor_log_->AddRecord(record);
    if (s.ok()) s = descriptor_file_->Sync();
  }
  if (s.ok() && !new_manifest.empty()) s = SetCurrentFile(dbname_, manifest_file_number_);
  lock.lock();

  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number_;
  } else if (!new_manifest.empty()) {
    descriptor_log_.reset();
    descriptor_file_.reset();
    RemoveFile(new_manifest);
  }
  return s;
}

Status VersionSet::Recover() {
  std::string current;
  Status s = ReadFileToString(CurrentFileName(dbname_), &current);
  if (!s.ok()) return s;
  if (current.empty() || current.back() != '\n') return Status::Corruption("o arquivo CURRENT não termina com nova linha");
  current.pop_back();
  std::unique_ptr<SequentialFile> file;
  s = SequentialFile::Open(JoinPath(dbname_, current), &file);
  if (!s.ok()) return s;

  bool have_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t log_number = 0;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  Version empty(table_cache_, icmp_);
  Builder builder(&empty);
  {
    CorruptionReporter reporter(&s);
    wal::Reader reader(file.get(), &reporter);
    std::string_view record;
    std::string scratch;
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (!s.ok()) break;
      builder.Apply(&edit, compact_pointer_);
      if (edit.has_log_number_) {
        log_number = edit.log_number_;
        have_log_number = true;
      }
      if (edit.has_next_file_number_) {
        next_file = edit.next_file_number_;
        have_next_file = true;
      }
      if (edit.has_last_sequence_) {
        last_sequence = edit.last_sequence_;
        have_last_sequence = true;
      }
    }
  }
  if (!s.ok()) return s;
  if (!have_next_file) return Status::Corruption("MANIFEST sem o próximo número de arquivo");
  if (!have_log_number) return Status::Corruption("MANIFEST sem o número do log");
  if (!have_last_sequence) return Status::Corruption("MANIFEST sem a última sequência");

  MarkFileNumberUsed(log_number);
  auto v = std::make_shared<Version>(table_cache_, icmp_);
  builder.SaveTo(v.get(), *icmp_);
  Finalize(v.get());
  AppendVersion(v);
  manifest_file_number_ = next_file;
  next_file_number_ = std::max(next_file_number_, next_file + 1);
  log_number_ = log_number;
  SetLastSequence(last_sequence);
  return Status::OK();
}

void VersionSet::GetRange(const FileList& inputs, InternalKey* smallest, InternalKey* largest) const {
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_->Compare(f->smallest.Encode(), smallest->Encode()) < 0) *smallest = f->smallest;
      if (icmp_->Compare(f->largest.Encode(), largest->Encode()) > 0) *largest = f->largest;
    }
  }
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest;
  InternalKey largest;
  GetRange(c->inputs_[0], &smallest, &largest);
  c->input_version_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);
  FileList all = c->inputs_[0];
  all.insert(all.end(), c->inputs_[1].begin(), c->inputs_[1].end());
  InternalKey all_start;
  InternalKey all_limit;
  GetRange(all, &all_start, &all_limit);
  if (level + 2 < kNumLevels) c->input_version_->GetOverlappingInputs(level + 2, &all_start, &all_limit, &c->grandparents_);
  compact_pointer_[level] = std::string(largest.Encode());
  c->edit()->SetCompactPointer(level, largest);
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() {
  if (current_->compaction_score_ < 1) return nullptr;
  const int level = current_->compaction_level_;
  auto c = std::make_unique<Compaction>(options_, level);
  c->reason_ = level == 0 ? "L0" : "size";
  for (const auto& f : current_->files_[level]) {
    if (compact_pointer_[level].empty() || icmp_->Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
      c->inputs_[0].push_back(f);
      break;
    }
  }
  if (c->inputs_[0].empty()) {
    if (current_->files_[level].empty()) return nullptr;
    c->inputs_[0].push_back(current_->files_[level][0]);
  }
  c->input_version_ = current_;
  if (level == 0) {
    InternalKey smallest;
    InternalKey largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
  }
  SetupOtherInputs(c.get());
  return c;
}

std::unique_ptr<Compaction> VersionSet::CompactLevel(int level) {
  if (level < 0 || level >= kNumLevels - 1 || current_->files_[level].empty()) return nullptr;
  auto c = std::make_unique<Compaction>(options_, level);
  c->reason_ = "manual";
  c->input_version_ = current_;
  c->inputs_[0] = current_->files_[level];
  SetupOtherInputs(c.get());
  return c;
}

std::unique_ptr<Iterator> VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;
  std::vector<std::unique_ptr<Iterator>> iters;
  for (int which = 0; which < 2; ++which) {
    if (c->inputs_[which].empty()) continue;
    if (c->level() + which == 0) {
      for (const auto& f : c->inputs_[which]) iters.push_back(table_cache_->NewIterator(options, f->number));
    } else {
      iters.push_back(NewConcatenatingIterator(icmp_, table_cache_, c->inputs_[which], options));
    }
  }
  return NewMergingIterator(icmp_, std::move(iters));
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (const auto& w : live_versions_) {
    auto v = w.lock();
    if (!v) continue;
    for (int level = 0; level < kNumLevels; ++level) {
      for (const auto& f : v->files_[level]) live->insert(f->number);
    }
  }
}

}
