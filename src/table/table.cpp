#include "table/table.hpp"

#include <algorithm>
#include <chrono>

#include "table/bloom.hpp"
#include "table/iterators.hpp"
#include "util/coding.hpp"
#include "util/hash.hpp"

namespace arca {

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval),
      index_block_(1) {
  props_.smallest_seq = kMaxSequenceNumber;
  props_.bloom_bits_per_key = static_cast<uint64_t>(options.bloom_bits_per_key);
}

void TableBuilder::Add(std::string_view internal_key, std::string_view value) {
  if (!status_.ok() || closed_) return;
  if (pending_index_entry_) {
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(last_key_, handle_encoding);
    pending_index_entry_ = false;
  }
  ParsedInternalKey parsed;
  if (ParseInternalKey(internal_key, &parsed)) {
    if (filter_keys_.empty() || parsed.user_key != last_user_key_) {
      filter_keys_.emplace_back(parsed.user_key);
      last_user_key_.assign(parsed.user_key.data(), parsed.user_key.size());
    }
    if (parsed.type == kTypeDeletion) ++props_.deletions;
    props_.smallest_seq = std::min(props_.smallest_seq, parsed.sequence);
    props_.largest_seq = std::max(props_.largest_seq, parsed.sequence);
  }
  last_key_.assign(internal_key.data(), internal_key.size());
  ++props_.entries;
  props_.raw_key_bytes += internal_key.size();
  props_.raw_value_bytes += value.size();
  data_block_.Add(internal_key, value);
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) FlushDataBlock();
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty() || !status_.ok()) return;
  WriteBlock(data_block_.Finish(), &pending_handle_);
  data_block_.Reset();
  ++props_.data_blocks;
  pending_index_entry_ = true;
}

void TableBuilder::WriteBlock(std::string_view contents, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = contents.size();
  status_ = file_->Append(contents);
  if (!status_.ok()) return;
  char trailer[kBlockTrailerSize];
  trailer[0] = 0;
  uint32_t crc = crc32c::Value(contents.data(), contents.size());
  crc = crc32c::Extend(crc, trailer, 1);
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  status_ = file_->Append(std::string_view(trailer, kBlockTrailerSize));
  if (status_.ok()) offset_ += contents.size() + kBlockTrailerSize;
}

Status TableBuilder::Finish() {
  FlushDataBlock();
  closed_ = true;
  if (props_.entries == 0) props_.smallest_seq = 0;
  props_.created_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  Footer footer;
  if (status_.ok()) {
    std::string filter;
    BloomFilter(options_.bloom_bits_per_key).Create(filter_keys_, &filter);
    WriteBlock(filter, &footer.filter);
  }
  if (status_.ok()) {
    std::string props;
    props_.EncodeTo(&props);
    WriteBlock(props, &footer.properties);
  }
  if (status_.ok()) {
    if (pending_index_entry_) {
      std::string handle_encoding;
      pending_handle_.EncodeTo(&handle_encoding);
      index_block_.Add(last_key_, handle_encoding);
      pending_index_entry_ = false;
    }
    WriteBlock(index_block_.Finish(), &footer.index);
  }
  if (status_.ok()) {
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    status_ = file_->Append(footer_encoding);
    if (status_.ok()) offset_ += footer_encoding.size();
  }
  return status_;
}

Status Table::Open(const Options& options, std::unique_ptr<RandomAccessFile> file, BlockCache* cache,
                   EngineCounters* counters, std::shared_ptr<Table>* table) {
  uint64_t size = file->size();
  if (size < Footer::kEncodedLength) return Status::Corruption("arquivo pequeno demais para ser uma SSTable");
  std::string footer_data;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_data);
  if (!s.ok()) return s;
  Footer footer;
  s = footer.DecodeFrom(footer_data);
  if (!s.ok()) return s;
  std::string index_contents;
  s = ReadBlock(file.get(), true, footer.index, &index_contents);
  if (!s.ok()) return s;
  std::string props_contents;
  s = ReadBlock(file.get(), true, footer.properties, &props_contents);
  if (!s.ok()) return s;
  std::string filter_contents;
  s = ReadBlock(file.get(), true, footer.filter, &filter_contents);
  if (!s.ok()) return s;
  std::shared_ptr<Table> t(new Table());
  t->file_ = std::move(file);
  t->cache_ = cache;
  t->counters_ = counters;
  t->cache_id_ = cache != nullptr ? cache->NewId() : 0;
  t->index_block_ = std::make_shared<const Block>(std::move(index_contents));
  t->filter_ = std::move(filter_contents);
  s = t->props_.DecodeFrom(props_contents);
  if (!s.ok()) return s;
  (void)options;
  *table = std::move(t);
  return Status::OK();
}

std::shared_ptr<const Block> Table::GetBlock(const ReadOptions& options, std::string_view handle_value, Status* s) const {
  BlockHandle handle;
  std::string_view input = handle_value;
  *s = handle.DecodeFrom(&input);
  if (!s->ok()) return nullptr;
  if (cache_ != nullptr) {
    auto cached = cache_->Lookup(cache_id_, handle.offset);
    if (cached) return cached;
  }
  std::string contents;
  *s = ReadBlock(file_.get(), true, handle, &contents);
  if (!s->ok()) return nullptr;
  auto block = std::make_shared<const Block>(std::move(contents));
  if (cache_ != nullptr && options.fill_cache) cache_->Insert(cache_id_, handle.offset, block);
  return block;
}

std::unique_ptr<Iterator> Table::NewIterator(const ReadOptions& options) const {
  std::shared_ptr<const Table> self = shared_from_this();
  return NewTwoLevelIterator(NewBlockIterator(index_block_, &icmp_), [self, options](std::string_view handle_value) {
    Status s;
    auto block = self->GetBlock(options, handle_value, &s);
    if (!s.ok()) return NewErrorIterator(s);
    return NewBlockIterator(block, &self->icmp_);
  });
}

Status Table::InternalGet(const ReadOptions& options, std::string_view internal_key,
                          const std::function<void(std::string_view, std::string_view)>& handle) const {
  if (counters_ != nullptr) counters_->bloom_checks.fetch_add(1, std::memory_order_relaxed);
  if (!BloomFilter::KeyMayMatch(ExtractUserKey(internal_key), filter_)) {
    if (counters_ != nullptr) counters_->bloom_useful.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
  }
  auto index_iter = NewBlockIterator(index_block_, &icmp_);
  index_iter->Seek(internal_key);
  if (!index_iter->Valid()) return index_iter->status();
  Status s;
  auto block = GetBlock(options, index_iter->value(), &s);
  if (!s.ok()) return s;
  auto block_iter = NewBlockIterator(block, &icmp_);
  block_iter->Seek(internal_key);
  if (block_iter->Valid()) handle(block_iter->key(), block_iter->value());
  return block_iter->status();
}

Status Table::VerifyAll() const {
  ReadOptions options;
  options.fill_cache = false;
  auto it = NewIterator(options);
  std::string prev;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (!prev.empty() && icmp_.Compare(prev, it->key()) >= 0) return Status::Corruption("chaves fora de ordem");
    prev.assign(it->key().data(), it->key().size());
  }
  return it->status();
}

}
