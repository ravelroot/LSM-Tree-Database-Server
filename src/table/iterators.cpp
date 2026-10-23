#include "table/iterators.hpp"

#include <string>

namespace arca {

namespace {

class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(Status s) : status_(std::move(s)) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view) override {}
  void Next() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override { return status_; }

 private:
  Status status_;
};

class MergingIterator final : public Iterator {
 public:
  MergingIterator(const Comparator* cmp, std::vector<std::unique_ptr<Iterator>> children)
      : comparator_(cmp), children_(std::move(children)) {}

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    FindSmallest();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    FindSmallest();
  }

  void Next() override {
    current_->Next();
    FindSmallest();
  }

  std::string_view key() const override { return current_->key(); }
  std::string_view value() const override { return current_->value(); }

  Status status() const override {
    for (const auto& c : children_) {
      Status s = c->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  void FindSmallest() {
    Iterator* smallest = nullptr;
    for (auto& c : children_) {
      if (!c->Valid()) continue;
      if (smallest == nullptr || comparator_->Compare(c->key(), smallest->key()) < 0) smallest = c.get();
    }
    current_ = smallest;
  }

  const Comparator* comparator_;
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
};

class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index_iter, BlockFunction fn)
      : index_iter_(std::move(index_iter)), block_function_(std::move(fn)) {}

  bool Valid() const override { return data_iter_ != nullptr && data_iter_->Valid(); }
  std::string_view key() const override { return data_iter_->key(); }
  std::string_view value() const override { return data_iter_->value(); }

  void Seek(std::string_view target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_) data_iter_->Seek(target);
    SkipEmptyDataBlocksForward();
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_) data_iter_->SeekToFirst();
    SkipEmptyDataBlocksForward();
  }

  void Next() override {
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  Status status() const override {
    Status s = index_iter_->status();
    if (!s.ok()) return s;
    if (data_iter_) {
      s = data_iter_->status();
      if (!s.ok()) return s;
    }
    return status_;
  }

 private:
  void SetDataIterator(std::unique_ptr<Iterator> it) {
    if (data_iter_ && status_.ok()) {
      Status s = data_iter_->status();
      if (!s.ok()) status_ = s;
    }
    data_iter_ = std::move(it);
  }

  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    std::string_view handle = index_iter_->value();
    if (data_iter_ && handle == data_block_handle_) return;
    data_block_handle_.assign(handle.data(), handle.size());
    SetDataIterator(block_function_(handle));
  }

  void SkipEmptyDataBlocksForward() {
    while (!data_iter_ || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_) data_iter_->SeekToFirst();
    }
  }

  std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> data_iter_;
  BlockFunction block_function_;
  std::string data_block_handle_;
  Status status_;
};

}

std::unique_ptr<Iterator> NewErrorIterator(Status status) { return std::make_unique<EmptyIterator>(std::move(status)); }

std::unique_ptr<Iterator> NewEmptyIterator() { return std::make_unique<EmptyIterator>(Status::OK()); }

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* cmp, std::vector<std::unique_ptr<Iterator>> children) {
  if (children.empty()) return NewEmptyIterator();
  if (children.size() == 1) return std::move(children[0]);
  return std::make_unique<MergingIterator>(cmp, std::move(children));
}

std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function) {
  return std::make_unique<TwoLevelIterator>(std::move(index_iter), std::move(block_function));
}

}
