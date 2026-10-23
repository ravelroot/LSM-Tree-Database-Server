#include "util/file.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace arca {

namespace {

Status ErrnoStatus(const std::string& context, int err) {
  if (err == ENOENT) return Status::NotFound(context + ": " + std::strerror(err));
  return Status::IOError(context + ": " + std::strerror(err));
}

Status FsStatus(const std::string& context, const std::error_code& ec) {
  return Status::IOError(context + ": " + ec.message());
}

constexpr size_t kWriteBufferSize = 64 * 1024;

#ifdef _WIN32
int OsOpen(const std::string& path, int flags) { return _open(path.c_str(), flags | _O_BINARY, _S_IREAD | _S_IWRITE); }
long long OsWrite(int fd, const char* data, size_t n) { return _write(fd, data, static_cast<unsigned>(n)); }
long long OsRead(int fd, char* data, size_t n) { return _read(fd, data, static_cast<unsigned>(n)); }
int OsClose(int fd) { return _close(fd); }
int OsSync(int fd) { return _commit(fd); }
constexpr int kWriteFlags = _O_WRONLY | _O_CREAT;
constexpr int kTruncFlag = _O_TRUNC;
constexpr int kAppendFlag = _O_APPEND;
constexpr int kReadFlags = _O_RDONLY;
#else
int OsOpen(const std::string& path, int flags) { return ::open(path.c_str(), flags | O_CLOEXEC, 0644); }
long long OsWrite(int fd, const char* data, size_t n) { return ::write(fd, data, n); }
long long OsRead(int fd, char* data, size_t n) { return ::read(fd, data, n); }
int OsClose(int fd) { return ::close(fd); }
int OsSync(int fd) {
#if defined(__APPLE__)
  return ::fsync(fd);
#else
  return ::fdatasync(fd);
#endif
}
constexpr int kWriteFlags = O_WRONLY | O_CREAT;
constexpr int kTruncFlag = O_TRUNC;
constexpr int kAppendFlag = O_APPEND;
constexpr int kReadFlags = O_RDONLY;
#endif

}

WritableFile::WritableFile(std::string path, int fd, uint64_t size) : path_(std::move(path)), fd_(fd), size_(size) {
  buffer_.reserve(kWriteBufferSize);
}

WritableFile::~WritableFile() { Close(); }

Status WritableFile::Open(const std::string& path, bool truncate, std::unique_ptr<WritableFile>* result) {
  int flags = kWriteFlags | (truncate ? kTruncFlag : kAppendFlag);
  int fd = OsOpen(path, flags);
  if (fd < 0) return ErrnoStatus(path, errno);
  uint64_t size = 0;
  if (!truncate) {
    std::error_code ec;
    auto s = fs::file_size(path, ec);
    if (!ec) size = s;
  }
  result->reset(new WritableFile(path, fd, size));
  return Status::OK();
}

Status WritableFile::WriteUnbuffered(const char* data, size_t n) {
  while (n > 0) {
    long long written = OsWrite(fd_, data, n);
    if (written < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus(path_, errno);
    }
    data += written;
    n -= static_cast<size_t>(written);
  }
  return Status::OK();
}

Status WritableFile::Append(std::string_view data) {
  if (fd_ < 0) return Status::IOError(path_ + ": arquivo fechado");
  size_ += data.size();
  if (buffer_.size() + data.size() <= kWriteBufferSize) {
    buffer_.append(data.data(), data.size());
    return Status::OK();
  }
  Status s = Flush();
  if (!s.ok()) return s;
  if (data.size() <= kWriteBufferSize) {
    buffer_.append(data.data(), data.size());
    return Status::OK();
  }
  return WriteUnbuffered(data.data(), data.size());
}

Status WritableFile::Flush() {
  if (buffer_.empty()) return Status::OK();
  Status s = WriteUnbuffered(buffer_.data(), buffer_.size());
  buffer_.clear();
  return s;
}

Status WritableFile::Sync() {
  Status s = Flush();
  if (!s.ok()) return s;
  if (OsSync(fd_) != 0) return ErrnoStatus(path_, errno);
  return Status::OK();
}

Status WritableFile::Close() {
  if (fd_ < 0) return Status::OK();
  Status s = Flush();
  if (OsClose(fd_) != 0 && s.ok()) s = ErrnoStatus(path_, errno);
  fd_ = -1;
  return s;
}

SequentialFile::SequentialFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}

SequentialFile::~SequentialFile() {
  if (fd_ >= 0) OsClose(fd_);
}

Status SequentialFile::Open(const std::string& path, std::unique_ptr<SequentialFile>* result) {
  int fd = OsOpen(path, kReadFlags);
  if (fd < 0) return ErrnoStatus(path, errno);
  result->reset(new SequentialFile(path, fd));
  return Status::OK();
}

Status SequentialFile::Read(size_t n, std::string* scratch, std::string_view* result) {
  scratch->resize(n);
  size_t total = 0;
  while (total < n) {
    long long r = OsRead(fd_, scratch->data() + total, n - total);
    if (r < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus(path_, errno);
    }
    if (r == 0) break;
    total += static_cast<size_t>(r);
  }
  *result = std::string_view(scratch->data(), total);
  return Status::OK();
}

#ifdef _WIN32
struct RandomAccessFile::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
};

Status RandomAccessFile::Open(const std::string& path, std::unique_ptr<RandomAccessFile>* result) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return Status::IOError(path + ": não foi possível abrir");
  LARGE_INTEGER size;
  if (!GetFileSizeEx(h, &size)) {
    CloseHandle(h);
    return Status::IOError(path + ": tamanho desconhecido");
  }
  auto impl = std::make_unique<Impl>();
  impl->handle = h;
  result->reset(new RandomAccessFile(path, std::move(impl), static_cast<uint64_t>(size.QuadPart)));
  return Status::OK();
}

RandomAccessFile::~RandomAccessFile() {
  if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) CloseHandle(impl_->handle);
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, std::string* scratch) const {
  scratch->resize(n);
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD read = 0;
  if (!ReadFile(impl_->handle, scratch->data(), static_cast<DWORD>(n), &read, &ov) && GetLastError() != ERROR_HANDLE_EOF) {
    return Status::IOError(path_ + ": falha de leitura");
  }
  scratch->resize(read);
  return Status::OK();
}

struct FileLock::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
};

Status FileLock::Acquire(const std::string& path, std::unique_ptr<FileLock>* result) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return Status::Busy(path + ": o diretório já está em uso por outro processo");
  auto impl = std::make_unique<Impl>();
  impl->handle = h;
  result->reset(new FileLock(std::move(impl)));
  return Status::OK();
}

FileLock::~FileLock() {
  if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) CloseHandle(impl_->handle);
}

Status SyncDir(const std::string&) { return Status::OK(); }
#else
struct RandomAccessFile::Impl {
  int fd = -1;
};

Status RandomAccessFile::Open(const std::string& path, std::unique_ptr<RandomAccessFile>* result) {
  int fd = OsOpen(path, kReadFlags);
  if (fd < 0) return ErrnoStatus(path, errno);
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    int err = errno;
    OsClose(fd);
    return ErrnoStatus(path, err);
  }
  auto impl = std::make_unique<Impl>();
  impl->fd = fd;
  result->reset(new RandomAccessFile(path, std::move(impl), static_cast<uint64_t>(st.st_size)));
  return Status::OK();
}

RandomAccessFile::~RandomAccessFile() {
  if (impl_ && impl_->fd >= 0) OsClose(impl_->fd);
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, std::string* scratch) const {
  scratch->resize(n);
  size_t total = 0;
  while (total < n) {
    ssize_t r = ::pread(impl_->fd, scratch->data() + total, n - total, static_cast<off_t>(offset + total));
    if (r < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus(path_, errno);
    }
    if (r == 0) break;
    total += static_cast<size_t>(r);
  }
  scratch->resize(total);
  return Status::OK();
}

struct FileLock::Impl {
  int fd = -1;
};

Status FileLock::Acquire(const std::string& path, std::unique_ptr<FileLock>* result) {
  int fd = OsOpen(path, O_RDWR | O_CREAT);
  if (fd < 0) return ErrnoStatus(path, errno);
  struct flock fl {};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (fcntl(fd, F_SETLK, &fl) != 0) {
    OsClose(fd);
    return Status::Busy(path + ": o diretório já está em uso por outro processo");
  }
  auto impl = std::make_unique<Impl>();
  impl->fd = fd;
  result->reset(new FileLock(std::move(impl)));
  return Status::OK();
}

FileLock::~FileLock() {
  if (impl_ && impl_->fd >= 0) OsClose(impl_->fd);
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return ErrnoStatus(dir, errno);
  int rc = ::fsync(fd);
  int err = errno;
  ::close(fd);
  if (rc != 0) return ErrnoStatus(dir, err);
  return Status::OK();
}
#endif

RandomAccessFile::RandomAccessFile(std::string path, std::unique_ptr<Impl> impl, uint64_t size)
    : path_(std::move(path)), impl_(std::move(impl)), size_(size) {}

FileLock::FileLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Status CreateDirs(const std::string& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) return FsStatus(path, ec);
  return Status::OK();
}

Status GetChildren(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) names->push_back(entry.path().filename().string());
  if (ec) return FsStatus(dir, ec);
  return Status::OK();
}

Status RemoveFile(const std::string& path) {
  std::error_code ec;
  fs::remove(path, ec);
  if (ec) return FsStatus(path, ec);
  return Status::OK();
}

Status RenameFile(const std::string& from, const std::string& to) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (ec) return FsStatus(from, ec);
  return Status::OK();
}

Status GetFileSize(const std::string& path, uint64_t* size) {
  std::error_code ec;
  auto s = fs::file_size(path, ec);
  if (ec) return FsStatus(path, ec);
  *size = s;
  return Status::OK();
}

bool FileExists(const std::string& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

Status ReadFileToString(const std::string& path, std::string* data) {
  std::unique_ptr<SequentialFile> file;
  Status s = SequentialFile::Open(path, &file);
  if (!s.ok()) return s;
  data->clear();
  std::string scratch;
  while (true) {
    std::string_view chunk;
    s = file->Read(8192, &scratch, &chunk);
    if (!s.ok()) return s;
    if (chunk.empty()) break;
    data->append(chunk.data(), chunk.size());
  }
  return Status::OK();
}

Status WriteStringToFileSync(std::string_view data, const std::string& path) {
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(path, true, &file);
  if (!s.ok()) return s;
  s = file->Append(data);
  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  if (!s.ok()) RemoveFile(path);
  return s;
}

Status LinkOrCopyFile(const std::string& from, const std::string& to) {
  std::error_code ec;
  fs::create_hard_link(from, to, ec);
  if (!ec) return Status::OK();
  ec.clear();
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) return FsStatus(from, ec);
  return Status::OK();
}

std::string JoinPath(const std::string& dir, const std::string& name) { return (fs::path(dir) / name).string(); }

}
