#pragma once

#include <string>
#include <string_view>

namespace arca {

class Status {
 public:
  enum class Code { kOk, kNotFound, kCorruption, kInvalidArgument, kIOError, kBusy, kNotSupported };

  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg = {}) { return Status(Code::kNotFound, msg); }
  static Status Corruption(std::string_view msg) { return Status(Code::kCorruption, msg); }
  static Status InvalidArgument(std::string_view msg) { return Status(Code::kInvalidArgument, msg); }
  static Status IOError(std::string_view msg) { return Status(Code::kIOError, msg); }
  static Status Busy(std::string_view msg) { return Status(Code::kBusy, msg); }
  static Status NotSupported(std::string_view msg) { return Status(Code::kNotSupported, msg); }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsBusy() const { return code_ == Code::kBusy; }
  bool IsNotSupported() const { return code_ == Code::kNotSupported; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  std::string ToString() const {
    const char* label = "OK";
    switch (code_) {
      case Code::kOk:
        return "OK";
      case Code::kNotFound:
        label = "não encontrado";
        break;
      case Code::kCorruption:
        label = "dados corrompidos";
        break;
      case Code::kInvalidArgument:
        label = "argumento inválido";
        break;
      case Code::kIOError:
        label = "erro de E/S";
        break;
      case Code::kBusy:
        label = "ocupado";
        break;
      case Code::kNotSupported:
        label = "não suportado";
        break;
    }
    if (msg_.empty()) return label;
    return std::string(label) + ": " + msg_;
  }

 private:
  Status(Code code, std::string_view msg) : code_(code), msg_(msg) {}

  Code code_ = Code::kOk;
  std::string msg_;
};

}
