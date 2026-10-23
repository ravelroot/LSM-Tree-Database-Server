#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "arca/status.hpp"

namespace arca::server {

struct Config {
  std::string bind = "127.0.0.1";
  int port = 7379;
  std::string dir = "dados";
  std::string requirepass;
  int io_threads = 2;
  int worker_threads = 4;
  std::string appendfsync = "everysec";
  uint64_t write_buffer_size = 4ull << 20;
  uint64_t max_file_size = 2ull << 20;
  uint64_t block_cache_size = 64ull << 20;
  uint64_t level1_max_bytes = 10ull << 20;
  int bloom_bits_per_key = 10;
  int maxclients = 10000;
  int timeout = 0;
  uint64_t proto_max_bulk_len = 64ull << 20;
  int64_t slowlog_log_slower_than = 10000;
  int slowlog_max_len = 128;
  std::string loglevel = "info";

  Status LoadFile(const std::string& path);
  Status Set(const std::string& name, const std::string& value);
  void ApplyEnvironment();
  std::vector<std::pair<std::string, std::string>> All() const;
};

bool ParseBytes(const std::string& text, uint64_t* out);

}
