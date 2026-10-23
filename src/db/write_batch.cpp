#include "arca/write_batch.hpp"

#include "db/dbformat.hpp"
#include "db/memtable.hpp"
#include "db/write_batch_internal.hpp"
#include "util/coding.hpp"

namespace arca {

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(WriteBatchInternal::kHeader);
}

uint32_t WriteBatch::Count() const { return WriteBatchInternal::Count(this); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixed(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) { WriteBatchInternal::Append(this, &source); }

Status WriteBatch::Iterate(Handler* handler) const {
  std::string_view input(rep_);
  if (input.size() < WriteBatchInternal::kHeader) return Status::Corruption("lote de escrita pequeno demais");
  input.remove_prefix(WriteBatchInternal::kHeader);
  uint32_t found = 0;
  while (!input.empty()) {
    ++found;
    char tag = input[0];
    input.remove_prefix(1);
    std::string_view key;
    std::string_view value;
    switch (tag) {
      case kTypeValue:
        if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
          return Status::Corruption("registro de escrita inválido");
        }
        handler->Put(key, value);
        break;
      case kTypeDeletion:
        if (!GetLengthPrefixed(&input, &key)) return Status::Corruption("registro de remoção inválido");
        handler->Delete(key);
        break;
      default:
        return Status::Corruption("tipo de registro desconhecido no lote");
    }
  }
  if (found != Count()) return Status::Corruption("contagem do lote não confere");
  return Status::OK();
}

uint32_t WriteBatchInternal::Count(const WriteBatch* batch) { return DecodeFixed32(batch->rep_.data() + 8); }

void WriteBatchInternal::SetCount(WriteBatch* batch, uint32_t n) { EncodeFixed32(batch->rep_.data() + 8, n); }

uint64_t WriteBatchInternal::Sequence(const WriteBatch* batch) { return DecodeFixed64(batch->rep_.data()); }

void WriteBatchInternal::SetSequence(WriteBatch* batch, uint64_t seq) { EncodeFixed64(batch->rep_.data(), seq); }

void WriteBatchInternal::SetContents(WriteBatch* batch, std::string_view contents) {
  batch->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

namespace {

class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(uint64_t seq, MemTable* mem) : sequence_(seq), mem_(mem) {}
  void Put(std::string_view key, std::string_view value) override { mem_->Add(sequence_++, kTypeValue, key, value); }
  void Delete(std::string_view key) override { mem_->Add(sequence_++, kTypeDeletion, key, {}); }

 private:
  uint64_t sequence_;
  MemTable* mem_;
};

}

Status WriteBatchInternal::InsertInto(const WriteBatch* batch, MemTable* memtable) {
  MemTableInserter inserter(Sequence(batch), memtable);
  return batch->Iterate(&inserter);
}

}
