#include "server/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace arca::server {

namespace {

std::string Trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool ParseInt(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = v;
  return true;
}

std::string FormatBytes(uint64_t v) {
  if (v % (1ull << 30) == 0 && v > 0) return std::to_string(v >> 30) + "gb";
  if (v % (1ull << 20) == 0 && v > 0) return std::to_string(v >> 20) + "mb";
  if (v % (1ull << 10) == 0 && v > 0) return std::to_string(v >> 10) + "kb";
  return std::to_string(v);
}

}

bool ParseBytes(const std::string& text, uint64_t* out) {
  std::string t;
  for (char c : text) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  uint64_t mult = 1;
  auto ends = [&](const char* suf) {
    size_t n = std::char_traits<char>::length(suf);
    return t.size() > n && t.compare(t.size() - n, n, suf) == 0;
  };
  if (ends("gb")) {
    mult = 1ull << 30;
    t.resize(t.size() - 2);
  } else if (ends("mb")) {
    mult = 1ull << 20;
    t.resize(t.size() - 2);
  } else if (ends("kb")) {
    mult = 1ull << 10;
    t.resize(t.size() - 2);
  } else if (ends("b")) {
    t.resize(t.size() - 1);
  }
  int64_t v = 0;
  if (!ParseInt(t, &v) || v < 0) return false;
  *out = static_cast<uint64_t>(v) * mult;
  return true;
}

Status Config::Set(const std::string& raw_name, const std::string& value) {
  std::string name;
  for (char c : raw_name) name.push_back(c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  int64_t n = 0;
  uint64_t bytes = 0;
  auto bad = [&](const char* what) { return Status::InvalidArgument("valor inválido para " + name + ": " + what); };
  if (name == "bind") {
    bind = value;
  } else if (name == "port") {
    if (!ParseInt(value, &n) || n < 0 || n > 65535) return bad("use uma porta entre 0 e 65535");
    port = static_cast<int>(n);
  } else if (name == "dir") {
    dir = value;
  } else if (name == "requirepass") {
    requirepass = value;
  } else if (name == "io-threads") {
    if (!ParseInt(value, &n) || n < 1 || n > 64) return bad("use de 1 a 64");
    io_threads = static_cast<int>(n);
  } else if (name == "worker-threads") {
    if (!ParseInt(value, &n) || n < 1 || n > 256) return bad("use de 1 a 256");
    worker_threads = static_cast<int>(n);
  } else if (name == "appendfsync") {
    if (value != "always" && value != "everysec" && value != "no") return bad("use always, everysec ou no");
    appendfsync = value;
  } else if (name == "write-buffer-size") {
    if (!ParseBytes(value, &bytes) || bytes < (64u << 10)) return bad("mínimo 64kb");
    write_buffer_size = bytes;
  } else if (name == "max-file-size") {
    if (!ParseBytes(value, &bytes) || bytes < (16u << 10)) return bad("mínimo 16kb");
    max_file_size = bytes;
  } else if (name == "block-cache-size") {
    if (!ParseBytes(value, &bytes)) return bad("tamanho em bytes, kb, mb ou gb");
    block_cache_size = bytes;
  } else if (name == "level1-max-bytes") {
    if (!ParseBytes(value, &bytes) || bytes < (64u << 10)) return bad("mínimo 64kb");
    level1_max_bytes = bytes;
  } else if (name == "bloom-bits-per-key") {
    if (!ParseInt(value, &n) || n < 1 || n > 32) return bad("use de 1 a 32");
    bloom_bits_per_key = static_cast<int>(n);
  } else if (name == "maxclients") {
    if (!ParseInt(value, &n) || n < 1) return bad("use um número positivo");
    maxclients = static_cast<int>(n);
  } else if (name == "timeout") {
    if (!ParseInt(value, &n) || n < 0) return bad("segundos, 0 desliga");
    timeout = static_cast<int>(n);
  } else if (name == "proto-max-bulk-len") {
    if (!ParseBytes(value, &bytes) || bytes < 1024) return bad("mínimo 1kb");
    proto_max_bulk_len = bytes;
  } else if (name == "slowlog-log-slower-than") {
    if (!ParseInt(value, &n)) return bad("microssegundos");
    slowlog_log_slower_than = n;
  } else if (name == "slowlog-max-len") {
    if (!ParseInt(value, &n) || n < 0) return bad("número de entradas");
    slowlog_max_len = static_cast<int>(n);
  } else if (name == "loglevel") {
    if (value != "debug" && value != "info" && value != "warning") return bad("use debug, info ou warning");
    loglevel = value;
  } else {
    return Status::InvalidArgument("parâmetro desconhecido: " + raw_name);
  }
  return Status::OK();
}

Status Config::LoadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) return Status::IOError("não foi possível abrir " + path);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    size_t sp = line.find_first_of(" \t");
    std::string key = sp == std::string::npos ? line : line.substr(0, sp);
    std::string value = sp == std::string::npos ? "" : Unquote(Trim(line.substr(sp)));
    Status s = Set(key, value);
    if (!s.ok()) return Status::InvalidArgument(path + ":" + std::to_string(number) + ": " + s.message());
  }
  return Status::OK();
}

void Config::ApplyEnvironment() {
  static const std::pair<const char*, const char*> vars[] = {
      {"ARCA_BIND", "bind"},         {"ARCA_PORT", "port"},           {"ARCA_DIR", "dir"},
      {"ARCA_PASSWORD", "requirepass"}, {"ARCA_IO_THREADS", "io-threads"}, {"ARCA_WORKER_THREADS", "worker-threads"},
      {"ARCA_APPENDFSYNC", "appendfsync"}, {"ARCA_BLOCK_CACHE_SIZE", "block-cache-size"},
      {"ARCA_WRITE_BUFFER_SIZE", "write-buffer-size"}};
  for (const auto& [env, name] : vars) {
    const char* v = std::getenv(env);
    if (v != nullptr && *v != '\0') Set(name, v);
  }
}

std::vector<std::pair<std::string, std::string>> Config::All() const {
  return {{"bind", bind},
          {"port", std::to_string(port)},
          {"dir", dir},
          {"requirepass", requirepass},
          {"io-threads", std::to_string(io_threads)},
          {"worker-threads", std::to_string(worker_threads)},
          {"appendfsync", appendfsync},
          {"write-buffer-size", FormatBytes(write_buffer_size)},
          {"max-file-size", FormatBytes(max_file_size)},
          {"block-cache-size", FormatBytes(block_cache_size)},
          {"level1-max-bytes", FormatBytes(level1_max_bytes)},
          {"bloom-bits-per-key", std::to_string(bloom_bits_per_key)},
          {"maxclients", std::to_string(maxclients)},
          {"timeout", std::to_string(timeout)},
          {"proto-max-bulk-len", FormatBytes(proto_max_bulk_len)},
          {"slowlog-log-slower-than", std::to_string(slowlog_log_slower_than)},
          {"slowlog-max-len", std::to_string(slowlog_max_len)},
          {"loglevel", loglevel}};
}

}
