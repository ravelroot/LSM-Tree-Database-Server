#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "arca/status.hpp"

namespace arca {

enum class FileType { kLog, kTable, kManifest, kCurrent, kLock, kTemp };

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string ManifestFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);
bool ParseFileName(std::string_view name, uint64_t* number, FileType* type);
Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number);

}
