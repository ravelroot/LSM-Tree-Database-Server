#include "table/bloom.hpp"

#include <algorithm>

#include "util/hash.hpp"

namespace arca {

namespace {

uint32_t BloomHash(std::string_view key) { return Hash(key, 0xbc9f1d34u); }

}

BloomFilter::BloomFilter(int bits_per_key) : bits_per_key_(static_cast<size_t>(std::max(1, bits_per_key))) {
  k_ = static_cast<size_t>(static_cast<double>(bits_per_key_) * 0.69);
  k_ = std::clamp<size_t>(k_, 1, 30);
}

void BloomFilter::Create(const std::vector<std::string>& keys, std::string* dst) const {
  size_t bits = std::max<size_t>(64, keys.size() * bits_per_key_);
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  size_t init_size = dst->size();
  dst->resize(init_size + bytes, 0);
  dst->push_back(static_cast<char>(k_));
  char* array = dst->data() + init_size;
  for (const auto& key : keys) {
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k_; ++j) {
      const uint32_t bitpos = static_cast<uint32_t>(h % bits);
      array[bitpos / 8] = static_cast<char>(array[bitpos / 8] | (1 << (bitpos % 8)));
      h += delta;
    }
  }
}

bool BloomFilter::KeyMayMatch(std::string_view key, std::string_view filter) {
  const size_t len = filter.size();
  if (len < 2) return true;
  const char* array = filter.data();
  const size_t bits = (len - 1) * 8;
  const size_t k = static_cast<unsigned char>(array[len - 1]);
  if (k > 30) return true;
  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (size_t j = 0; j < k; ++j) {
    const uint32_t bitpos = static_cast<uint32_t>(h % bits);
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

}
