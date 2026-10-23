#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "arca/status.hpp"

namespace arca {

class WritableFile {
 public:
  static Status Open(const std::string& path, bool truncate, std::unique_ptr<WritableFile>* result);
  ~WritableFile();
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  Status Append(std::string_view data);
  Status Flush();
  Status Sync();
  Status Close();
  uint64_t Size() const { return size_; }
  const std::string& path() const { return path_; }

 private:
  WritableFile(std::string path, int fd, uint64_t size);
  Status WriteUnbuffered(const char* data, size_t n);

  std::string path_;
  int fd_;
  uint64_t size_;
  std::string buffer_;
};

class SequentialFile {
 public:
  static Status Open(const std::string& path, std::unique_ptr<SequentialFile>* result);
  ~SequentialFile();
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  Status Read(size_t n, std::string* scratch, std::string_view* result);

 private:
  SequentialFile(std::string path, int fd);
  std::string path_;
  int fd_;
};

class RandomAccessFile {
 public:
  static Status Open(const std::string& path, std::unique_ptr<RandomAccessFile>* result);
  ~RandomAccessFile();
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  Status Read(uint64_t offset, size_t n, std::string* scratch) const;
  uint64_t size() const { return size_; }

 private:
  struct Impl;
  RandomAccessFile(std::string path, std::unique_ptr<Impl> impl, uint64_t size);
  std::string path_;
  std::unique_ptr<Impl> impl_;
  uint64_t size_;
};

class FileLock {
 public:
  static Status Acquire(const std::string& path, std::unique_ptr<FileLock>* result);
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  struct Impl;
  explicit FileLock(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

Status CreateDirs(const std::string& path);
Status GetChildren(const std::string& dir, std::vector<std::string>* names);
Status RemoveFile(const std::string& path);
Status RenameFile(const std::string& from, const std::string& to);
Status GetFileSize(const std::string& path, uint64_t* size);
Status ReadFileToString(const std::string& path, std::string* data);
Status WriteStringToFileSync(std::string_view data, const std::string& path);
Status SyncDir(const std::string& dir);
Status LinkOrCopyFile(const std::string& from, const std::string& to);
bool FileExists(const std::string& path);
std::string JoinPath(const std::string& dir, const std::string& name);

}
