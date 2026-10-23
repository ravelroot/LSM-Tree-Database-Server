#include "db/version_edit.hpp"

#include <cstdio>

#include "util/coding.hpp"

namespace arca {

namespace {

enum Tag : uint32_t {
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
};

bool GetInternalKey(std::string_view* input, InternalKey* dst) {
  std::string_view str;
  if (!GetLengthPrefixed(input, &str)) return false;
  dst->DecodeFrom(str);
  return true;
}

bool GetLevel(std::string_view* input, int* level) {
  uint32_t v = 0;
  if (!GetVarint32(input, &v) || v >= static_cast<uint32_t>(kNumLevels)) return false;
  *level = static_cast<int>(v);
  return true;
}

std::string Printable(std::string_view s) {
  std::string out;
  for (unsigned char c : s) {
    if (c >= 32 && c < 127 && c != '\\') {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

}

void VersionEdit::Clear() { *this = VersionEdit(); }

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutLengthPrefixed(dst, key.Encode());
  }
  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, f] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutVarint64(dst, f.entries);
    PutVarint64(dst, f.deletions);
    PutLengthPrefixed(dst, f.smallest.Encode());
    PutLengthPrefixed(dst, f.largest.Encode());
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  Clear();
  std::string_view input = src;
  const char* msg = nullptr;
  uint32_t tag = 0;
  int level = 0;
  uint64_t number = 0;
  FileMetaData f;
  InternalKey key;
  while (msg == nullptr && GetVarint32(&input, &tag)) {
    switch (tag) {
      case kLogNumber:
        if (GetVarint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          msg = "número do log";
        }
        break;
      case kNextFileNumber:
        if (GetVarint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          msg = "próximo número de arquivo";
        }
        break;
      case kLastSequence:
        if (GetVarint64(&input, &last_sequence_)) {
          has_last_sequence_ = true;
        } else {
          msg = "última sequência";
        }
        break;
      case kCompactPointer:
        if (GetLevel(&input, &level) && GetInternalKey(&input, &key)) {
          compact_pointers_.emplace_back(level, key);
        } else {
          msg = "ponteiro de compactação";
        }
        break;
      case kDeletedFile:
        if (GetLevel(&input, &level) && GetVarint64(&input, &number)) {
          deleted_files_.insert({level, number});
        } else {
          msg = "arquivo removido";
        }
        break;
      case kNewFile:
        if (GetLevel(&input, &level) && GetVarint64(&input, &f.number) && GetVarint64(&input, &f.file_size) &&
            GetVarint64(&input, &f.entries) && GetVarint64(&input, &f.deletions) &&
            GetInternalKey(&input, &f.smallest) && GetInternalKey(&input, &f.largest)) {
          new_files_.emplace_back(level, f);
        } else {
          msg = "novo arquivo";
        }
        break;
      default:
        msg = "marcador desconhecido";
        break;
    }
  }
  if (msg == nullptr && !input.empty()) msg = "bytes sobrando";
  if (msg != nullptr) return Status::Corruption(std::string("registro do MANIFEST inválido: ") + msg);
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::string r = "{";
  if (has_log_number_) r += " log=" + std::to_string(log_number_);
  if (has_next_file_number_) r += " proximo_arquivo=" + std::to_string(next_file_number_);
  if (has_last_sequence_) r += " ultima_sequencia=" + std::to_string(last_sequence_);
  for (const auto& [level, k] : compact_pointers_) {
    r += " ponteiro[L" + std::to_string(level) + "]=" + Printable(k.user_key());
  }
  for (const auto& [level, number] : deleted_files_) {
    r += " remove[L" + std::to_string(level) + "]=" + std::to_string(number);
  }
  for (const auto& [level, f] : new_files_) {
    r += " adiciona[L" + std::to_string(level) + "]=" + std::to_string(f.number) + ":" + std::to_string(f.file_size) +
         "B[" + Printable(f.smallest.user_key()) + " .. " + Printable(f.largest.user_key()) + "]";
  }
  r += " }";
  return r;
}

}
