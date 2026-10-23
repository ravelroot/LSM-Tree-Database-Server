#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace arca {

class Snapshot;

enum class SyncMode { kAlways, kEverySecond, kNever };

struct Options {
  bool create_if_missing = true;
  bool error_if_exists = false;
  bool paranoid_checks = false;
  size_t write_buffer_size = 4u << 20;
  size_t max_file_size = 2u << 20;
  size_t block_size = 4096;
  int block_restart_interval = 16;
  int bloom_bits_per_key = 10;
  size_t block_cache_capacity = 64u << 20;
  int max_open_files = 500;
  int level0_compaction_trigger = 4;
  int level0_slowdown_trigger = 8;
  int level0_stop_trigger = 12;
  uint64_t level1_max_bytes = 10u << 20;
  int level_size_multiplier = 10;
  SyncMode sync_mode = SyncMode::kEverySecond;
  std::function<void(std::string_view)> info_log;
};

struct ReadOptions {
  const Snapshot* snapshot = nullptr;
  bool verify_checksums = false;
  bool fill_cache = true;
};

struct WriteOptions {
  bool sync = false;
};

}
