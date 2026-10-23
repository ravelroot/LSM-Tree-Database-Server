#include "db/log.hpp"

#include "util/coding.hpp"
#include "util/hash.hpp"

namespace arca::wal {

Writer::Writer(WritableFile* dest, uint64_t dest_length) : dest_(dest), block_offset_(dest_length % kBlockSize) {
  for (int i = 0; i <= kMaxRecordType; ++i) {
    char t = static_cast<char>(i);
    type_crc_[i] = crc32c::Value(&t, 1);
  }
}

Status Writer::AddRecord(std::string_view record) {
  const char* ptr = record.data();
  size_t left = record.size();
  Status s;
  bool begin = true;
  do {
    const size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      if (leftover > 0) {
        static const char kZeros[kHeaderSize] = {0, 0, 0, 0, 0, 0, 0};
        s = dest_->Append(std::string_view(kZeros, leftover));
        if (!s.ok()) return s;
      }
      block_offset_ = 0;
    }
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment_length = left < avail ? left : avail;
    const bool end = left == fragment_length;
    RecordType type;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }
    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  if (s.ok()) s = dest_->Flush();
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* data, size_t length) {
  char header[kHeaderSize];
  header[4] = static_cast<char>(length & 0xff);
  header[5] = static_cast<char>(length >> 8);
  header[6] = static_cast<char>(t);
  uint32_t crc = crc32c::Extend(type_crc_[t], data, length);
  EncodeFixed32(header, crc32c::Mask(crc));
  Status s = dest_->Append(std::string_view(header, kHeaderSize));
  if (s.ok()) s = dest_->Append(std::string_view(data, length));
  block_offset_ += kHeaderSize + length;
  return s;
}

Reader::Reader(SequentialFile* file, Reporter* reporter) : file_(file), reporter_(reporter) {}

void Reader::ReportCorruption(size_t bytes, const char* reason) {
  if (reporter_ != nullptr) reporter_->Corruption(bytes, Status::Corruption(reason));
}

bool Reader::ReadRecord(std::string_view* record, std::string* scratch) {
  scratch->clear();
  *record = {};
  bool in_fragmented_record = false;
  uint64_t prospective_record_offset = 0;
  std::string_view fragment;
  while (true) {
    const unsigned int record_type = ReadPhysicalRecord(&fragment);
    uint64_t physical_record_offset = end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();
    switch (record_type) {
      case kFullType:
        if (in_fragmented_record && !scratch->empty()) ReportCorruption(scratch->size(), "registro parcial sem fim");
        prospective_record_offset = physical_record_offset;
        scratch->clear();
        *record = fragment;
        last_record_offset_ = prospective_record_offset;
        return true;
      case kFirstType:
        if (in_fragmented_record && !scratch->empty()) ReportCorruption(scratch->size(), "registro parcial sem fim");
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;
      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "fragmento do meio sem início");
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;
      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "fragmento final sem início");
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = *scratch;
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;
      case kEof:
        scratch->clear();
        return false;
      case kBadRecord:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "erro no meio de um registro");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;
      default:
        ReportCorruption(fragment.size() + (in_fragmented_record ? scratch->size() : 0), "tipo de registro desconhecido");
        in_fragmented_record = false;
        scratch->clear();
        break;
    }
  }
}

unsigned int Reader::ReadPhysicalRecord(std::string_view* result) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        buffer_ = {};
        Status status = file_->Read(kBlockSize, &backing_store_, &buffer_);
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) {
          buffer_ = {};
          ReportCorruption(kBlockSize, "falha de leitura do log");
          eof_ = true;
          return kEof;
        }
        if (buffer_.size() < kBlockSize) eof_ = true;
        continue;
      }
      buffer_ = {};
      return kEof;
    }
    const char* header = buffer_.data();
    const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
    const unsigned int type = static_cast<unsigned char>(header[6]);
    const uint32_t length = a | (b << 8);
    if (kHeaderSize + length > buffer_.size()) {
      size_t drop_size = buffer_.size();
      buffer_ = {};
      if (!eof_) {
        ReportCorruption(drop_size, "tamanho de registro inválido");
        return kBadRecord;
      }
      return kEof;
    }
    if (type == kZeroType && length == 0) {
      buffer_ = {};
      return kBadRecord;
    }
    uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
    uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
    if (actual_crc != expected_crc) {
      size_t drop_size = buffer_.size();
      buffer_ = {};
      ReportCorruption(drop_size, "checksum incorreto");
      return kBadRecord;
    }
    buffer_.remove_prefix(kHeaderSize + length);
    *result = std::string_view(header + kHeaderSize, length);
    return type;
  }
}

}
