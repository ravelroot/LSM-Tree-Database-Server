#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "arca/status.hpp"
#include "util/file.hpp"

namespace arca {

inline constexpr uint64_t kTableMagic = 0x4152434153535442ull;
inline constexpr size_t kBlockTrailerSize = 5;

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;
  static constexpr size_t kMaxEncodedLength = 20;
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view* input);
};

struct Footer {
  BlockHandle filter;
  BlockHandle properties;
  BlockHandle index;
  static constexpr size_t kEncodedLength = 3 * BlockHandle::kMaxEncodedLength + 8;
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view input);
};

struct TableProperties {
  uint64_t entries = 0;
  uint64_t deletions = 0;
  uint64_t data_blocks = 0;
  uint64_t raw_key_bytes = 0;
  uint64_t raw_value_bytes = 0;
  uint64_t smallest_seq = 0;
  uint64_t largest_seq = 0;
  uint64_t created_ms = 0;
  uint64_t bloom_bits_per_key = 0;
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view input);
};

Status ReadBlock(const RandomAccessFile* file, bool verify_checksum, const BlockHandle& handle, std::string* contents);

}
