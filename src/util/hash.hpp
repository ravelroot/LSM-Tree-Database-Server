#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arca {

namespace crc32c {

uint32_t Extend(uint32_t init, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

inline constexpr uint32_t kMaskDelta = 0xa282ead8u;

inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }

inline uint32_t Unmask(uint32_t masked) {
  uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

}

uint32_t Hash(const char* data, size_t n, uint32_t seed);

inline uint32_t Hash(std::string_view s, uint32_t seed) { return Hash(s.data(), s.size(), seed); }

}
