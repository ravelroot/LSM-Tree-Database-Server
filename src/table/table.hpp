#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "arca/iterator.hpp"
#include "arca/options.hpp"
#include "db/dbformat.hpp"
#include "table/block.hpp"
#include "table/cache.hpp"
#include "table/format.hpp"
#include "util/file.hpp"

namespace arca {

class TableBuilder {
 public:
  TableBuilder(const Options& options, WritableFile* file);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  void Add(std::string_view internal_key, std::string_view value);
  Status Finish();
  uint64_t NumEntries() const { return props_.entries; }
  uint64_t NumDeletions() const { return props_.deletions; }
  uint64_t FileSize() const { return offset_; }
  Status status() const { return status_; }

 private:
  void FlushDataBlock();
  void WriteBlock(std::string_view contents, BlockHandle* handle);

  const Options& options_;
  WritableFile* file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  std::string last_user_key_;
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;
  std::vector<std::string> filter_keys_;
  TableProperties props_;
  bool closed_ = false;
};

class Table : public std::enable_shared_from_this<Table> {
 public:
  static Status Open(const Options& options, std::unique_ptr<RandomAccessFile> file, BlockCache* cache,
                     EngineCounters* counters, std::shared_ptr<Table>* table);

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) const;
  Status InternalGet(const ReadOptions& options, std::string_view internal_key,
                     const std::function<void(std::string_view, std::string_view)>& handle) const;
  const TableProperties& properties() const { return props_; }
  uint64_t file_size() const { return file_->size(); }
  Status VerifyAll() const;

 private:
  Table() = default;
  std::shared_ptr<const Block> GetBlock(const ReadOptions& options, std::string_view handle_value, Status* s) const;

  std::unique_ptr<RandomAccessFile> file_;
  BlockCache* cache_ = nullptr;
  EngineCounters* counters_ = nullptr;
  uint64_t cache_id_ = 0;
  std::shared_ptr<const Block> index_block_;
  std::string filter_;
  TableProperties props_;
  InternalKeyComparator icmp_;
};

}
