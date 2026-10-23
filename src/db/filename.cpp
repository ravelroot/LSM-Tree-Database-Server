#include "db/filename.hpp"

#include <cstdio>

#include "util/file.hpp"

namespace arca {

namespace {

std::string NumberedName(const std::string& dbname, uint64_t number, const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return JoinPath(dbname, buf);
}

bool ParseNumber(std::string_view s, uint64_t* out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  *out = v;
  return true;
}

}

std::string LogFileName(const std::string& dbname, uint64_t number) { return NumberedName(dbname, number, "wal"); }

std::string TableFileName(const std::string& dbname, uint64_t number) { return NumberedName(dbname, number, "sst"); }

std::string TempFileName(const std::string& dbname, uint64_t number) { return NumberedName(dbname, number, "tmp"); }

std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "MANIFEST-%06llu", static_cast<unsigned long long>(number));
  return JoinPath(dbname, buf);
}

std::string CurrentFileName(const std::string& dbname) { return JoinPath(dbname, "CURRENT"); }

std::string LockFileName(const std::string& dbname) { return JoinPath(dbname, "LOCK"); }

bool ParseFileName(std::string_view name, uint64_t* number, FileType* type) {
  if (name == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrent;
    return true;
  }
  if (name == "LOCK") {
    *number = 0;
    *type = FileType::kLock;
    return true;
  }
  if (name.substr(0, 9) == "MANIFEST-") {
    if (!ParseNumber(name.substr(9), number)) return false;
    *type = FileType::kManifest;
    return true;
  }
  size_t dot = name.find('.');
  if (dot == std::string_view::npos) return false;
  if (!ParseNumber(name.substr(0, dot), number)) return false;
  std::string_view suffix = name.substr(dot + 1);
  if (suffix == "wal") {
    *type = FileType::kLog;
  } else if (suffix == "sst") {
    *type = FileType::kTable;
  } else if (suffix == "tmp") {
    *type = FileType::kTemp;
  } else {
    return false;
  }
  return true;
}

Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number) {
  std::string manifest = ManifestFileName(dbname, manifest_number);
  std::string contents = manifest.substr(manifest.find("MANIFEST-")) + "\n";
  std::string tmp = TempFileName(dbname, manifest_number);
  Status s = WriteStringToFileSync(contents, tmp);
  if (s.ok()) s = RenameFile(tmp, CurrentFileName(dbname));
  if (s.ok()) s = SyncDir(dbname);
  if (!s.ok()) RemoveFile(tmp);
  return s;
}

}
