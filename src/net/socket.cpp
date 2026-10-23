#include "net/socket.hpp"

#include <cerrno>
#include <cstring>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#endif
#endif

namespace arca::net {

namespace {

#ifdef _WIN32
bool LastErrorWouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
int LastErrorCode() { return WSAGetLastError(); }
#else
bool LastErrorWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
int LastErrorCode() { return errno; }
#endif

std::string ErrorText(const std::string& context) {
#ifdef _WIN32
  return context + ": erro de socket " + std::to_string(LastErrorCode());
#else
  return context + ": " + std::strerror(LastErrorCode());
#endif
}

addrinfo* Resolve(const std::string& host, int port, bool passive) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (passive) hints.ai_flags = AI_PASSIVE;
  addrinfo* result = nullptr;
  std::string service = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  if (getaddrinfo(node, service.c_str(), &hints, &result) != 0) return nullptr;
  return result;
}

}

bool Startup() {
#ifdef _WIN32
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  return true;
#endif
}

void CloseSocket(socket_t fd) {
  if (fd == kInvalidSocket) return;
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(fd));
#else
  ::close(fd);
#endif
}

bool SetNonBlocking(socket_t fd) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void SetNoDelay(socket_t fd) {
  int one = 1;
  setsockopt(static_cast<decltype(socket(0, 0, 0))>(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
             sizeof(one));
}

Status Listen(const std::string& host, int port, int backlog, socket_t* out) {
  addrinfo* res = Resolve(host, port, true);
  if (res == nullptr) return Status::IOError("não foi possível resolver o endereço " + host);
  Status last = Status::IOError("nenhum endereço disponível");
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    auto fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (static_cast<socket_t>(fd) == kInvalidSocket) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    if (bind(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) {
      last = Status::IOError(ErrorText("bind na porta " + std::to_string(port)));
      CloseSocket(static_cast<socket_t>(fd));
      continue;
    }
    if (listen(fd, backlog) != 0) {
      last = Status::IOError(ErrorText("listen"));
      CloseSocket(static_cast<socket_t>(fd));
      continue;
    }
    SetNonBlocking(static_cast<socket_t>(fd));
    *out = static_cast<socket_t>(fd);
    freeaddrinfo(res);
    return Status::OK();
  }
  freeaddrinfo(res);
  return last;
}

Status Connect(const std::string& host, int port, socket_t* out) {
  addrinfo* res = Resolve(host, port, false);
  if (res == nullptr) return Status::IOError("não foi possível resolver o endereço " + host);
  Status last = Status::IOError("nenhum endereço disponível");
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    auto fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (static_cast<socket_t>(fd) == kInvalidSocket) continue;
    if (connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) {
      last = Status::IOError(ErrorText("conexão com " + host + ":" + std::to_string(port)));
      CloseSocket(static_cast<socket_t>(fd));
      continue;
    }
    SetNoDelay(static_cast<socket_t>(fd));
    *out = static_cast<socket_t>(fd);
    freeaddrinfo(res);
    return Status::OK();
  }
  freeaddrinfo(res);
  return last;
}

socket_t Accept(socket_t listener, std::string* peer) {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  auto fd = accept(static_cast<decltype(socket(0, 0, 0))>(listener), reinterpret_cast<sockaddr*>(&addr), &len);
  if (static_cast<socket_t>(fd) == kInvalidSocket) return kInvalidSocket;
  char host[NI_MAXHOST] = {0};
  char serv[NI_MAXSERV] = {0};
  if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host), serv, sizeof(serv),
                  NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
    *peer = std::string(host) + ":" + serv;
  }
  return static_cast<socket_t>(fd);
}

long long SendSome(socket_t fd, const char* data, size_t n) {
#ifdef _WIN32
  int r = send(static_cast<SOCKET>(fd), data, static_cast<int>(n), 0);
#elif defined(MSG_NOSIGNAL)
  ssize_t r = send(fd, data, n, MSG_NOSIGNAL);
#else
  ssize_t r = send(fd, data, n, 0);
#endif
  if (r < 0) return LastErrorWouldBlock() ? kWouldBlock : -1;
  return static_cast<long long>(r);
}

long long RecvSome(socket_t fd, char* data, size_t n) {
#ifdef _WIN32
  int r = recv(static_cast<SOCKET>(fd), data, static_cast<int>(n), 0);
#else
  ssize_t r = recv(fd, data, n, 0);
#endif
  if (r < 0) return LastErrorWouldBlock() ? kWouldBlock : -1;
  return static_cast<long long>(r);
}

bool SendAll(socket_t fd, const char* data, size_t n) {
  while (n > 0) {
    long long r = SendSome(fd, data, n);
    if (r <= 0) return false;
    data += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

Waker::Waker() {
#ifdef _WIN32
  socket_t listener = kInvalidSocket;
  if (!Listen("127.0.0.1", 0, 1, &listener).ok()) return;
  sockaddr_in addr{};
  int len = sizeof(addr);
  getsockname(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&addr), &len);
  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  u_long mode = 0;
  ioctlsocket(static_cast<SOCKET>(listener), FIONBIO, &mode);
  SOCKET server = accept(static_cast<SOCKET>(listener), nullptr, nullptr);
  CloseSocket(listener);
  read_fd_ = static_cast<socket_t>(server);
  write_fd_ = static_cast<socket_t>(client);
  SetNonBlocking(read_fd_);
  SetNonBlocking(write_fd_);
#else
  int fds[2];
  if (pipe(fds) != 0) return;
  read_fd_ = fds[0];
  write_fd_ = fds[1];
  SetNonBlocking(read_fd_);
  SetNonBlocking(write_fd_);
#endif
}

Waker::~Waker() {
  CloseSocket(read_fd_);
  CloseSocket(write_fd_);
}

void Waker::Wake() {
  char c = 1;
#ifdef _WIN32
  send(static_cast<SOCKET>(write_fd_), &c, 1, 0);
#else
  ssize_t r = ::write(write_fd_, &c, 1);
  (void)r;
#endif
}

void Waker::Drain() {
  char buf[256];
  while (true) {
#ifdef _WIN32
    int r = recv(static_cast<SOCKET>(read_fd_), buf, sizeof(buf), 0);
#else
    ssize_t r = ::read(read_fd_, buf, sizeof(buf));
#endif
    if (r <= 0) break;
  }
}

#if defined(__linux__)
struct Poller::Impl {
  int epfd = -1;
  std::vector<epoll_event> buffer;
};

Poller::Poller() : impl_(new Impl) {
  impl_->epfd = epoll_create1(EPOLL_CLOEXEC);
  impl_->buffer.resize(256);
}

Poller::~Poller() {
  if (impl_->epfd >= 0) ::close(impl_->epfd);
  delete impl_;
}

namespace {
uint32_t ToEpoll(int events) {
  uint32_t e = 0;
  if (events & Poller::kRead) e |= EPOLLIN | EPOLLRDHUP;
  if (events & Poller::kWrite) e |= EPOLLOUT;
  return e;
}
}

bool Poller::Add(socket_t fd, int events) {
  epoll_event ev{};
  ev.events = ToEpoll(events);
  ev.data.fd = fd;
  return epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Poller::Modify(socket_t fd, int events) {
  epoll_event ev{};
  ev.events = ToEpoll(events);
  ev.data.fd = fd;
  return epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void Poller::Remove(socket_t fd) { epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, nullptr); }

int Poller::Wait(std::vector<Event>* events, int timeout_ms) {
  events->clear();
  int n = epoll_wait(impl_->epfd, impl_->buffer.data(), static_cast<int>(impl_->buffer.size()), timeout_ms);
  for (int i = 0; i < n; ++i) {
    const auto& e = impl_->buffer[i];
    events->push_back(Event{e.data.fd, (e.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0, (e.events & EPOLLOUT) != 0,
                            (e.events & EPOLLERR) != 0});
  }
  return n;
}

const char* Poller::Backend() const { return "epoll"; }
#else
struct Poller::Impl {
  std::unordered_map<socket_t, int> interest;
#ifdef _WIN32
  std::vector<WSAPOLLFD> fds;
#else
  std::vector<pollfd> fds;
#endif
};

Poller::Poller() : impl_(new Impl) {}

Poller::~Poller() { delete impl_; }

bool Poller::Add(socket_t fd, int events) {
  impl_->interest[fd] = events;
  return true;
}

bool Poller::Modify(socket_t fd, int events) {
  impl_->interest[fd] = events;
  return true;
}

void Poller::Remove(socket_t fd) { impl_->interest.erase(fd); }

int Poller::Wait(std::vector<Event>* events, int timeout_ms) {
  events->clear();
  impl_->fds.clear();
  for (const auto& [fd, ev] : impl_->interest) {
    decltype(impl_->fds)::value_type p{};
    p.fd = static_cast<decltype(p.fd)>(fd);
    p.events = static_cast<short>(((ev & kRead) ? POLLIN : 0) | ((ev & kWrite) ? POLLOUT : 0));
    impl_->fds.push_back(p);
  }
#ifdef _WIN32
  int n = WSAPoll(impl_->fds.data(), static_cast<ULONG>(impl_->fds.size()), timeout_ms);
#else
  int n = ::poll(impl_->fds.data(), static_cast<nfds_t>(impl_->fds.size()), timeout_ms);
#endif
  if (n <= 0) return n;
  for (const auto& p : impl_->fds) {
    if (p.revents == 0) continue;
    events->push_back(Event{static_cast<socket_t>(p.fd), (p.revents & (POLLIN | POLLHUP)) != 0,
                            (p.revents & POLLOUT) != 0, (p.revents & (POLLERR | POLLNVAL)) != 0});
  }
  return static_cast<int>(events->size());
}

const char* Poller::Backend() const {
#ifdef _WIN32
  return "wsapoll";
#else
  return "poll";
#endif
}
#endif

}
