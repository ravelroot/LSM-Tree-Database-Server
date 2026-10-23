#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "arca/iterator.hpp"
#include "db/dbformat.hpp"

namespace arca {

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval);
  void Reset();
  void Add(std::string_view key, std::string_view value);
  std::string_view Finish();
  size_t CurrentSizeEstimate() const;
  bool empty() const { return buffer_.empty(); }

 private:
  int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

class Block {
 public:
  explicit Block(std::string contents);
  size_t size() const { return data_.size(); }
  bool corrupt() const { return corrupt_; }

  friend std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block, const Comparator* cmp);

 private:
  std::string data_;
  uint32_t restart_offset_ = 0;
  uint32_t num_restarts_ = 0;
  bool corrupt_ = false;
};

std::unique_ptr<Iterator> NewBlockIterator(std::shared_ptr<const Block> block, const Comparator* cmp);

}
