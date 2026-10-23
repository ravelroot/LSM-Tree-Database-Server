#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace arca {

class BloomFilter {
 public:
  explicit BloomFilter(int bits_per_key);
  void Create(const std::vector<std::string>& keys, std::string* dst) const;
  static bool KeyMayMatch(std::string_view key, std::string_view filter);

 private:
  size_t bits_per_key_;
  size_t k_;
};

}
