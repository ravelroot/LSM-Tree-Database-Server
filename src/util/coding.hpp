#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace arca {

inline void EncodeFixed32(char* dst, uint32_t value) {
  for (int i = 0; i < 4; ++i) dst[i] = static_cast<char>((value >> (8 * i)) & 0xff);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((value >> (8 * i)) & 0xff);
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const auto* p = reinterpret_cast<const unsigned char*>(ptr);
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  return static_cast<uint64_t>(DecodeFixed32(ptr)) | (static_cast<uint64_t>(DecodeFixed32(ptr + 4)) << 32);
}

inline void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  EncodeFixed32(buf, value);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  EncodeFixed64(buf, value);
  dst->append(buf, 8);
}

inline void PutBigEndian32(std::string* dst, uint32_t value) {
  for (int i = 3; i >= 0; --i) dst->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

inline void PutBigEndian64(std::string* dst, uint64_t value) {
  for (int i = 7; i >= 0; --i) dst->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

inline uint32_t DecodeBigEndian32(const char* ptr) {
  const auto* p = reinterpret_cast<const unsigned char*>(ptr);
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t DecodeBigEndian64(const char* ptr) {
  return (static_cast<uint64_t>(DecodeBigEndian32(ptr)) << 32) | DecodeBigEndian32(ptr + 4);
}

char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
void PutLengthPrefixed(std::string* dst, std::string_view value);
int VarintLength(uint64_t value);

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);
bool GetVarint32(std::string_view* input, uint32_t* value);
bool GetVarint64(std::string_view* input, uint64_t* value);
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);
bool GetFixed64(std::string_view* input, uint64_t* value);
bool GetFixed32(std::string_view* input, uint32_t* value);

}
