#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "arca/status.hpp"

namespace arca::net {

#ifdef _WIN32
using socket_t = uintptr_t;
inline constexpr socket_t kInvalidSocket = ~static_cast<uintptr_t>(0);
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

inline constexpr long long kWouldBlock = -2;

bool Startup();
void CloseSocket(socket_t fd);
bool SetNonBlocking(socket_t fd);
void SetNoDelay(socket_t fd);
Status Listen(const std::string& host, int port, int backlog, socket_t* out);
Status Connect(const std::string& host, int port, socket_t* out);
socket_t Accept(socket_t listener, std::string* peer);
long long SendSome(socket_t fd, const char* data, size_t n);
long long RecvSome(socket_t fd, char* data, size_t n);
bool SendAll(socket_t fd, const char* data, size_t n);

class Waker {
 public:
  Waker();
  ~Waker();
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;
  bool ok() const { return read_fd_ != kInvalidSocket; }
  socket_t read_fd() const { return read_fd_; }
  void Wake();
  void Drain();

 private:
  socket_t read_fd_ = kInvalidSocket;
  socket_t write_fd_ = kInvalidSocket;
};

class Poller {
 public:
  enum : int { kRead = 1, kWrite = 2 };

  struct Event {
    socket_t fd;
    bool readable;
    bool writable;
    bool error;
  };

  Poller();
  ~Poller();
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  bool Add(socket_t fd, int events);
  bool Modify(socket_t fd, int events);
  void Remove(socket_t fd);
  int Wait(std::vector<Event>* events, int timeout_ms);
  const char* Backend() const;

 private:
  struct Impl;
  Impl* impl_;
};

}
