#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "arca/status.hpp"
#include "net/socket.hpp"

namespace arca::resp {

enum class Parse { kOk, kIncomplete, kError };

Parse ParseCommand(std::string_view buffer, size_t max_bulk, size_t* consumed, std::vector<std::string>* args,
                   std::string* error);

void AppendSimple(std::string* out, std::string_view s);
void AppendError(std::string* out, std::string_view s);
void AppendInteger(std::string* out, int64_t v);
void AppendBulk(std::string* out, std::string_view s);
void AppendNull(std::string* out);
void AppendNullArray(std::string* out);
void AppendArray(std::string* out, size_t n);

struct Value {
  enum class Type { kSimple, kError, kInteger, kBulk, kNull, kArray };
  Type type = Type::kNull;
  std::string str;
  int64_t integer = 0;
  std::vector<Value> elements;
};

Parse ParseReply(std::string_view buffer, size_t* consumed, Value* out);
std::string Format(const Value& v, const std::string& indent = "");
std::string EncodeCommand(const std::vector<std::string>& args);

class Client {
 public:
  Client() = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Status Connect(const std::string& host, int port);
  Status Send(const std::vector<std::string>& args);
  Status SendRaw(const std::string& data);
  Status Read(Value* out);
  Status Call(const std::vector<std::string>& args, Value* out);
  void Close();
  bool connected() const { return fd_ != net::kInvalidSocket; }

 private:
  net::socket_t fd_ = net::kInvalidSocket;
  std::string buffer_;
};

}
