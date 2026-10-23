#include "net/resp.hpp"

#include <charconv>
#include <cstdio>

namespace arca::resp {

namespace {

constexpr size_t kMaxInlineSize = 64 * 1024;
constexpr int64_t kMaxArgs = 1024 * 1024;

bool ParseInt(std::string_view s, int64_t* out) {
  if (s.empty()) return false;
  auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::vector<std::string> SplitInline(std::string_view line, bool* ok) {
  std::vector<std::string> args;
  *ok = true;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) break;
    std::string arg;
    if (line[i] == '"' || line[i] == '\'') {
      char quote = line[i++];
      bool closed = false;
      while (i < line.size()) {
        char c = line[i++];
        if (c == '\\' && i < line.size() && quote == '"') {
          char n = line[i++];
          if (n == 'n') {
            arg.push_back('\n');
          } else if (n == 't') {
            arg.push_back('\t');
          } else if (n == 'r') {
            arg.push_back('\r');
          } else {
            arg.push_back(n);
          }
        } else if (c == quote) {
          closed = true;
          break;
        } else {
          arg.push_back(c);
        }
      }
      if (!closed) *ok = false;
    } else {
      while (i < line.size() && line[i] != ' ' && line[i] != '\t') arg.push_back(line[i++]);
    }
    args.push_back(std::move(arg));
  }
  return args;
}

}

Parse ParseCommand(std::string_view buf, size_t max_bulk, size_t* consumed, std::vector<std::string>* args,
                   std::string* error) {
  args->clear();
  if (buf.empty()) return Parse::kIncomplete;
  if (buf[0] != '*') {
    size_t nl = buf.find('\n');
    if (nl == std::string_view::npos) {
      if (buf.size() > kMaxInlineSize) {
        *error = "Protocol error: comando em linha grande demais";
        return Parse::kError;
      }
      return Parse::kIncomplete;
    }
    std::string_view line = buf.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    bool ok = true;
    *args = SplitInline(line, &ok);
    if (!ok) {
      *error = "Protocol error: aspas não fechadas";
      return Parse::kError;
    }
    *consumed = nl + 1;
    return Parse::kOk;
  }
  size_t pos = 0;
  size_t crlf = buf.find("\r\n", pos);
  if (crlf == std::string_view::npos) return Parse::kIncomplete;
  int64_t count = 0;
  if (!ParseInt(buf.substr(1, crlf - 1), &count) || count > kMaxArgs) {
    *error = "Protocol error: tamanho de array inválido";
    return Parse::kError;
  }
  pos = crlf + 2;
  for (int64_t i = 0; i < count; ++i) {
    if (pos >= buf.size()) return Parse::kIncomplete;
    if (buf[pos] != '$') {
      *error = "Protocol error: esperado '$'";
      return Parse::kError;
    }
    crlf = buf.find("\r\n", pos);
    if (crlf == std::string_view::npos) return Parse::kIncomplete;
    int64_t len = 0;
    if (!ParseInt(buf.substr(pos + 1, crlf - pos - 1), &len) || len < 0 || static_cast<size_t>(len) > max_bulk) {
      *error = "Protocol error: tamanho de bulk inválido";
      return Parse::kError;
    }
    pos = crlf + 2;
    if (buf.size() < pos + static_cast<size_t>(len) + 2) return Parse::kIncomplete;
    args->emplace_back(buf.substr(pos, static_cast<size_t>(len)));
    pos += static_cast<size_t>(len) + 2;
  }
  *consumed = pos;
  return Parse::kOk;
}

void AppendSimple(std::string* out, std::string_view s) {
  out->push_back('+');
  out->append(s.data(), s.size());
  out->append("\r\n");
}

void AppendError(std::string* out, std::string_view s) {
  out->push_back('-');
  for (char c : s) out->push_back(c == '\r' || c == '\n' ? ' ' : c);
  out->append("\r\n");
}

void AppendInteger(std::string* out, int64_t v) {
  out->push_back(':');
  out->append(std::to_string(v));
  out->append("\r\n");
}

void AppendBulk(std::string* out, std::string_view s) {
  out->push_back('$');
  out->append(std::to_string(s.size()));
  out->append("\r\n");
  out->append(s.data(), s.size());
  out->append("\r\n");
}

void AppendNull(std::string* out) { out->append("$-1\r\n"); }

void AppendNullArray(std::string* out) { out->append("*-1\r\n"); }

void AppendArray(std::string* out, size_t n) {
  out->push_back('*');
  out->append(std::to_string(n));
  out->append("\r\n");
}

std::string EncodeCommand(const std::vector<std::string>& args) {
  std::string out;
  AppendArray(&out, args.size());
  for (const auto& a : args) AppendBulk(&out, a);
  return out;
}

Parse ParseReply(std::string_view buf, size_t* consumed, Value* out) {
  if (buf.empty()) return Parse::kIncomplete;
  size_t crlf = buf.find("\r\n");
  if (crlf == std::string_view::npos) return Parse::kIncomplete;
  std::string_view line = buf.substr(1, crlf - 1);
  char t = buf[0];
  *out = Value();
  switch (t) {
    case '+':
      out->type = Value::Type::kSimple;
      out->str = std::string(line);
      *consumed = crlf + 2;
      return Parse::kOk;
    case '-':
      out->type = Value::Type::kError;
      out->str = std::string(line);
      *consumed = crlf + 2;
      return Parse::kOk;
    case ':':
      out->type = Value::Type::kInteger;
      if (!ParseInt(line, &out->integer)) return Parse::kError;
      *consumed = crlf + 2;
      return Parse::kOk;
    case '$': {
      int64_t len = 0;
      if (!ParseInt(line, &len)) return Parse::kError;
      if (len < 0) {
        out->type = Value::Type::kNull;
        *consumed = crlf + 2;
        return Parse::kOk;
      }
      size_t start = crlf + 2;
      if (buf.size() < start + static_cast<size_t>(len) + 2) return Parse::kIncomplete;
      out->type = Value::Type::kBulk;
      out->str = std::string(buf.substr(start, static_cast<size_t>(len)));
      *consumed = start + static_cast<size_t>(len) + 2;
      return Parse::kOk;
    }
    case '*': {
      int64_t n = 0;
      if (!ParseInt(line, &n)) return Parse::kError;
      if (n < 0) {
        out->type = Value::Type::kNull;
        *consumed = crlf + 2;
        return Parse::kOk;
      }
      out->type = Value::Type::kArray;
      size_t pos = crlf + 2;
      for (int64_t i = 0; i < n; ++i) {
        size_t used = 0;
        Value element;
        Parse p = ParseReply(buf.substr(pos), &used, &element);
        if (p != Parse::kOk) return p;
        out->elements.push_back(std::move(element));
        pos += used;
      }
      *consumed = pos;
      return Parse::kOk;
    }
    default:
      return Parse::kError;
  }
}

std::string Format(const Value& v, const std::string& indent) {
  switch (v.type) {
    case Value::Type::kSimple:
      return v.str;
    case Value::Type::kError:
      return "(erro) " + v.str;
    case Value::Type::kInteger:
      return "(inteiro) " + std::to_string(v.integer);
    case Value::Type::kNull:
      return "(nulo)";
    case Value::Type::kBulk: {
      std::string out = "\"";
      for (char c : v.str) {
        if (c == '"' || c == '\\') {
          out.push_back('\\');
          out.push_back(c);
        } else if (c == '\n') {
          out += "\\n";
        } else if (static_cast<unsigned char>(c) < 32) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
      }
      return out + "\"";
    }
    case Value::Type::kArray: {
      if (v.elements.empty()) return "(lista vazia)";
      std::string out;
      for (size_t i = 0; i < v.elements.size(); ++i) {
        std::string prefix = std::to_string(i + 1) + ") ";
        if (i > 0) out += "\n" + indent;
        out += prefix + Format(v.elements[i], indent + std::string(prefix.size(), ' '));
      }
      return out;
    }
  }
  return {};
}

Client::~Client() { Close(); }

void Client::Close() {
  net::CloseSocket(fd_);
  fd_ = net::kInvalidSocket;
  buffer_.clear();
}

Status Client::Connect(const std::string& host, int port) {
  Close();
  net::Startup();
  return net::Connect(host, port, &fd_);
}

Status Client::SendRaw(const std::string& data) {
  if (!net::SendAll(fd_, data.data(), data.size())) return Status::IOError("falha ao enviar para o servidor");
  return Status::OK();
}

Status Client::Send(const std::vector<std::string>& args) { return SendRaw(EncodeCommand(args)); }

Status Client::Read(Value* out) {
  while (true) {
    size_t used = 0;
    Parse p = ParseReply(buffer_, &used, out);
    if (p == Parse::kOk) {
      buffer_.erase(0, used);
      return Status::OK();
    }
    if (p == Parse::kError) return Status::Corruption("resposta inválida do servidor");
    char chunk[16384];
    long long r = net::RecvSome(fd_, chunk, sizeof(chunk));
    if (r <= 0) return Status::IOError("conexão encerrada pelo servidor");
    buffer_.append(chunk, static_cast<size_t>(r));
  }
}

Status Client::Call(const std::vector<std::string>& args, Value* out) {
  Status s = Send(args);
  if (!s.ok()) return s;
  return Read(out);
}

}
