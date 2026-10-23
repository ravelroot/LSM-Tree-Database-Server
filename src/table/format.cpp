#include "table/format.hpp"

#include "util/coding.hpp"
#include "util/hash.hpp"

namespace arca {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

Status BlockHandle::DecodeFrom(std::string_view* input) {
  if (GetVarint64(input, &offset) && GetVarint64(input, &size)) return Status::OK();
  return Status::Corruption("referência de bloco inválida");
}

void Footer::EncodeTo(std::string* dst) const {
  size_t start = dst->size();
  filter.EncodeTo(dst);
  properties.EncodeTo(dst);
  index.EncodeTo(dst);
  dst->resize(start + 3 * BlockHandle::kMaxEncodedLength);
  PutFixed64(dst, kTableMagic);
}

Status Footer::DecodeFrom(std::string_view input) {
  if (input.size() < kEncodedLength) return Status::Corruption("rodapé da SSTable curto demais");
  uint64_t magic = DecodeFixed64(input.data() + kEncodedLength - 8);
  if (magic != kTableMagic) return Status::Corruption("o arquivo não é uma SSTable da Arca");
  std::string_view handles = input.substr(0, 3 * BlockHandle::kMaxEncodedLength);
  Status s = filter.DecodeFrom(&handles);
  if (s.ok()) s = properties.DecodeFrom(&handles);
  if (s.ok()) s = index.DecodeFrom(&handles);
  return s;
}

void TableProperties::EncodeTo(std::string* dst) const {
  PutVarint64(dst, entries);
  PutVarint64(dst, deletions);
  PutVarint64(dst, data_blocks);
  PutVarint64(dst, raw_key_bytes);
  PutVarint64(dst, raw_value_bytes);
  PutVarint64(dst, smallest_seq);
  PutVarint64(dst, largest_seq);
  PutVarint64(dst, created_ms);
  PutVarint64(dst, bloom_bits_per_key);
}

Status TableProperties::DecodeFrom(std::string_view input) {
  if (GetVarint64(&input, &entries) && GetVarint64(&input, &deletions) && GetVarint64(&input, &data_blocks) &&
      GetVarint64(&input, &raw_key_bytes) && GetVarint64(&input, &raw_value_bytes) &&
      GetVarint64(&input, &smallest_seq) && GetVarint64(&input, &largest_seq) && GetVarint64(&input, &created_ms) &&
      GetVarint64(&input, &bloom_bits_per_key)) {
    return Status::OK();
  }
  return Status::Corruption("propriedades da SSTable inválidas");
}

Status ReadBlock(const RandomAccessFile* file, bool verify_checksum, const BlockHandle& handle, std::string* contents) {
  size_t n = static_cast<size_t>(handle.size);
  Status s = file->Read(handle.offset, n + kBlockTrailerSize, contents);
  if (!s.ok()) return s;
  if (contents->size() != n + kBlockTrailerSize) return Status::Corruption("bloco truncado");
  const char* data = contents->data();
  if (verify_checksum) {
    uint32_t expected = crc32c::Unmask(DecodeFixed32(data + n + 1));
    uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != expected) return Status::Corruption("checksum do bloco não confere");
  }
  if (data[n] != 0) return Status::Corruption("tipo de compressão desconhecido");
  contents->resize(n);
  return Status::OK();
}

}
