#include "util/hash.hpp"

#include "util/coding.hpp"

namespace arca {

namespace crc32c {

namespace {

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int j = 0; j < 8; ++j) crc = (crc & 1) ? (crc >> 1) ^ 0x82f63b78u : crc >> 1;
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}

uint32_t Extend(uint32_t init, const char* data, size_t n) {
  uint32_t crc = ~init;
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) crc = kTable[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

}

uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  const uint32_t m = 0xc6a4a793u;
  const uint32_t r = 24;
  const char* limit = data + n;
  uint32_t h = seed ^ static_cast<uint32_t>(n * m);
  while (data + 4 <= limit) {
    uint32_t w = DecodeFixed32(data);
    data += 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }
  switch (limit - data) {
    case 3:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<unsigned char>(data[0]);
      h *= m;
      h ^= (h >> r);
      break;
    default:
      break;
  }
  return h;
}

}
