#include "server/server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <set>

#include "util/coding.hpp"

namespace arca::server {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

WorkerPool::WorkerPool(int threads) {
  for (int i = 0; i < threads; ++i) {
    threads_.emplace_back([this] {
      while (true) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(mu_);
          cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
          if (stop_ && tasks_.empty()) return;
          task = std::move(tasks_.front());
          tasks_.pop_front();
        }
        task();
      }
    });
  }
}

WorkerPool::~WorkerPool() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
}

void WorkerPool::Submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
}

size_t WorkerPool::Pending() {
  std::lock_guard<std::mutex> lock(mu_);
  return tasks_.size();
}

EventLoop::EventLoop(Server* server, int index) : server_(server), index_(index) {}

void EventLoop::Start() {
  poller_.Add(waker_.read_fd(), net::Poller::kRead);
  if (listener_ != net::kInvalidSocket) poller_.Add(listener_, net::Poller::kRead);
  thread_ = std::thread([this] { Run(); });
}

void EventLoop::Stop() {
  stop_.store(true);
  waker_.Wake();
}

void EventLoop::Join() {
  if (thread_.joinable()) thread_.join();
}

void EventLoop::AddConnection(net::socket_t fd, std::string peer) {
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    pending_.emplace_back(fd, std::move(peer));
  }
  waker_.Wake();
}

void EventLoop::Complete(std::shared_ptr<Connection> conn, std::string reply) {
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    completions_.emplace_back(std::move(conn), std::move(reply));
  }
  waker_.Wake();
}

void EventLoop::AcceptAll() {
  while (true) {
    std::string peer;
    net::socket_t fd = net::Accept(listener_, &peer);
    if (fd == net::kInvalidSocket) return;
    if (!server_->AcceptingMore()) {
      static const char kMsg[] = "-ERR número máximo de clientes atingido\r\n";
      net::SendSome(fd, kMsg, sizeof(kMsg) - 1);
      net::CloseSocket(fd);
      server_->CountRejected();
      continue;
    }
    int target = server_->next_loop_.fetch_add(1) % static_cast<int>(server_->loops_.size());
    server_->loops_[target]->AddConnection(fd, std::move(peer));
  }
}

void EventLoop::DrainQueues() {
  std::vector<std::pair<net::socket_t, std::string>> pending;
  std::vector<std::pair<std::shared_ptr<Connection>, std::string>> completions;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    pending.swap(pending_);
    completions.swap(completions_);
  }
  for (auto& [fd, peer] : pending) {
    net::SetNonBlocking(fd);
    net::SetNoDelay(fd);
    auto conn = std::make_shared<Connection>();
    conn->id = server_->NextConnectionId();
    conn->fd = fd;
    conn->loop = index_;
    conn->addr = std::move(peer);
    conn->created_ms = NowMs();
    conn->last_ms.store(conn->created_ms);
    conn->session.authenticated = server_->config().requirepass.empty();
    conns_[fd] = conn;
    poller_.Add(fd, net::Poller::kRead);
    server_->RegisterConnection(conn);
  }
  for (auto& [conn, reply] : completions) {
    if (conn->closed) continue;
    conn->busy = false;
    conn->out += reply;
    ProcessInput(conn);
  }
}

void EventLoop::CheckTimeouts() {
  int timeout = server_->config().timeout;
  if (timeout <= 0) return;
  int64_t now = NowMs();
  if (now - last_timeout_check_ < 1000) return;
  last_timeout_check_ = now;
  std::vector<std::shared_ptr<Connection>> idle;
  for (auto& [fd, conn] : conns_) {
    if (!conn->busy && !conn->session.in_multi && now - conn->last_ms.load() > static_cast<int64_t>(timeout) * 1000) {
      idle.push_back(conn);
    }
  }
  for (auto& conn : idle) Close(conn);
}

void EventLoop::Run() {
  std::vector<net::Poller::Event> events;
  while (!stop_.load()) {
    poller_.Wait(&events, 250);
    for (const auto& ev : events) {
      if (ev.fd == waker_.read_fd()) {
        waker_.Drain();
        continue;
      }
      if (ev.fd == listener_) {
        AcceptAll();
        continue;
      }
      auto it = conns_.find(ev.fd);
      if (it == conns_.end()) continue;
      auto conn = it->second;
      if (ev.error) {
        Close(conn);
        continue;
      }
      if (ev.readable) HandleRead(conn);
      if (!conn->closed && ev.writable) Flush(conn);
    }
    DrainQueues();
    CheckTimeouts();
  }
  std::vector<std::shared_ptr<Connection>> all;
  for (auto& [fd, conn] : conns_) all.push_back(conn);
  for (auto& conn : all) Close(conn);
}

void EventLoop::HandleRead(const std::shared_ptr<Connection>& conn) {
  char buf[16384];
  while (true) {
    long long r = net::RecvSome(conn->fd, buf, sizeof(buf));
    if (r == net::kWouldBlock) break;
    if (r <= 0) {
      Close(conn);
      return;
    }
    conn->in.append(buf, static_cast<size_t>(r));
    server_->AddNetInput(static_cast<size_t>(r));
    if (static_cast<size_t>(r) < sizeof(buf)) break;
  }
  conn->last_ms.store(NowMs());
  ProcessInput(conn);
}

void EventLoop::ProcessInput(const std::shared_ptr<Connection>& conn) {
  while (!conn->busy && !conn->closing && conn->in_offset < conn->in.size()) {
    size_t consumed = 0;
    Args args;
    std::string error;
    std::string_view view(conn->in);
    view.remove_prefix(conn->in_offset);
    auto p = resp::ParseCommand(view, server_->config().proto_max_bulk_len, &consumed, &args, &error);
    if (p == resp::Parse::kIncomplete) break;
    if (p == resp::Parse::kError) {
      resp::AppendError(&conn->out, "ERR " + error);
      conn->closing = true;
      break;
    }
    conn->in_offset += consumed;
    if (args.empty()) continue;
    if (server_->Execute(conn, args, &conn->out) == Server::Dispatch::kAsync) {
      conn->busy = true;
      break;
    }
  }
  if (conn->in_offset > 0 && (conn->in_offset == conn->in.size() || conn->in_offset > (1u << 20))) {
    conn->in.erase(0, conn->in_offset);
    conn->in_offset = 0;
  }
  Flush(conn);
}

void EventLoop::Flush(const std::shared_ptr<Connection>& conn) {
  if (conn->closed) return;
  while (conn->out_offset < conn->out.size()) {
    long long r = net::SendSome(conn->fd, conn->out.data() + conn->out_offset, conn->out.size() - conn->out_offset);
    if (r == net::kWouldBlock) break;
    if (r <= 0) {
      Close(conn);
      return;
    }
    conn->out_offset += static_cast<size_t>(r);
    server_->AddNetOutput(static_cast<size_t>(r));
  }
  if (conn->out_offset >= conn->out.size()) {
    conn->out.clear();
    conn->out_offset = 0;
    if (conn->writing) {
      poller_.Modify(conn->fd, net::Poller::kRead);
      conn->writing = false;
    }
    if (conn->closing) Close(conn);
  } else if (!conn->writing) {
    poller_.Modify(conn->fd, net::Poller::kRead | net::Poller::kWrite);
    conn->writing = true;
  }
}

void EventLoop::Close(const std::shared_ptr<Connection>& conn) {
  if (conn->closed) return;
  conn->closed = true;
  poller_.Remove(conn->fd);
  net::CloseSocket(conn->fd);
  conns_.erase(conn->fd);
  conn->session.snapshot.reset();
  server_->UnregisterConnection(conn->id);
}

Server::Server(Config config) : config_(std::move(config)) {}

Server::~Server() {
  stopping_.store(true);
  for (auto& loop : loops_) loop->Stop();
  for (auto& loop : loops_) loop->Join();
  if (expire_thread_.joinable()) expire_thread_.join();
  if (stats_thread_.joinable()) stats_thread_.join();
  workers_.reset();
  net::CloseSocket(listener_);
  if (db_ && shutdown_save_) db_->Flush();
  sql_.reset();
  db_.reset();
}

void Server::Log(const std::string& level, const std::string& message) {
  if (config_.loglevel == "warning" && level == "info") return;
  std::time_t t = std::time(nullptr);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  std::lock_guard<std::mutex> lock(log_mu_);
  std::fprintf(stderr, "%s [%s] %s\n", stamp, level.c_str(), message.c_str());
  std::fflush(stderr);
}

Status Server::Start() {
  net::Startup();
  start_ms_ = NowMs();
  Options options;
  options.write_buffer_size = static_cast<size_t>(config_.write_buffer_size);
  options.max_file_size = static_cast<size_t>(config_.max_file_size);
  options.block_cache_capacity = static_cast<size_t>(config_.block_cache_size);
  options.level1_max_bytes = config_.level1_max_bytes;
  options.bloom_bits_per_key = config_.bloom_bits_per_key;
  options.sync_mode = config_.appendfsync == "always" ? SyncMode::kAlways
                      : config_.appendfsync == "no"   ? SyncMode::kNever
                                                      : SyncMode::kEverySecond;
  options.info_log = [this](std::string_view msg) { Log("info", std::string(msg)); };
  Status s = DB::Open(options, config_.dir, &db_);
  if (!s.ok()) return s;
  sql_ = std::make_unique<sql::Engine>(db_.get());
  s = sql_->Load();
  if (!s.ok()) return s;
  s = net::Listen(config_.bind, config_.port, 511, &listener_);
  if (!s.ok()) return s;
  workers_ = std::make_unique<WorkerPool>(config_.worker_threads);
  for (int i = 0; i < config_.io_threads; ++i) loops_.push_back(std::make_unique<EventLoop>(this, i));
  loops_[0]->SetListener(listener_);
  for (auto& loop : loops_) loop->Start();
  expire_thread_ = std::thread([this] { ExpireLoop(); });
  stats_thread_ = std::thread([this] { StatsLoop(); });
  last_save_s_.store(NowMs() / 1000);
  net::Poller probe;
  Log("info", std::string("Arca ") + kVersion + " pronta em " + config_.bind + ":" + std::to_string(config_.port) +
                  " (dados em " + config_.dir + ", " + std::to_string(config_.io_threads) + " threads de E/S com " +
                  probe.Backend() + ", " + std::to_string(config_.worker_threads) + " workers, " +
                  std::to_string(sql_->TableCount()) + " tabela(s))");
  if (config_.requirepass.empty() && config_.bind != "127.0.0.1" && config_.bind != "localhost") {
    Log("warning", "o servidor aceita conexões externas sem senha; defina requirepass");
  }
  return Status::OK();
}

void Server::WaitForShutdown() {
  std::unique_lock<std::mutex> lock(shutdown_mu_);
  shutdown_cv_.wait(lock, [this] { return shutdown_requested_; });
}

void Server::RequestShutdown(bool save) {
  std::lock_guard<std::mutex> lock(shutdown_mu_);
  shutdown_requested_ = true;
  shutdown_save_ = save;
  shutdown_cv_.notify_all();
}

void Server::RegisterConnection(const std::shared_ptr<Connection>& conn) {
  connected_.fetch_add(1);
  total_connections_.fetch_add(1);
  std::lock_guard<std::mutex> lock(registry_mu_);
  registry_[conn->id] = conn;
}

void Server::UnregisterConnection(uint64_t id) {
  connected_.fetch_sub(1);
  std::lock_guard<std::mutex> lock(registry_mu_);
  registry_.erase(id);
}

std::vector<std::unique_lock<std::mutex>> Server::LockKeys(const std::vector<std::string>& keys) {
  std::set<size_t> slots;
  for (const auto& k : keys) slots.insert(std::hash<std::string>()(k) % stripes_.size());
  std::vector<std::unique_lock<std::mutex>> locks;
  for (size_t s : slots) locks.emplace_back(stripes_[s]);
  return locks;
}

void Server::StatsLoop() {
  uint64_t last = commands_.load();
  auto last_t = std::chrono::steady_clock::now();
  while (!stopping_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - last_t).count();
    if (secs < 1.0) continue;
    uint64_t current = commands_.load();
    ops_per_sec_.store(static_cast<double>(current - last) / secs);
    last = current;
    last_t = now;
  }
}

void Server::ExpireLoop() {
  while (!stopping_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int64_t now = NowMs();
    std::string start(1, '\x06');
    std::string limit(1, '\x06');
    PutBigEndian64(&limit, static_cast<uint64_t>(now) + 1);
    std::vector<std::pair<std::string, int64_t>> due;
    {
      auto it = db_->NewIterator({});
      for (it->Seek(start); it->Valid() && due.size() < 2000; it->Next()) {
        std::string_view k = it->key();
        if (k.empty() || k[0] != '\x06' || k.compare(limit) >= 0 || k.size() < 9) break;
        due.emplace_back(std::string(k), static_cast<int64_t>(DecodeBigEndian64(k.data() + 1)));
      }
    }
    if (due.empty()) continue;
    std::shared_lock<std::shared_mutex> exec(exec_mu_);
    std::vector<std::string> user_keys;
    for (const auto& entry : due) user_keys.push_back(entry.first.substr(9));
    auto locks = LockKeys(user_keys);
    WriteBatch batch;
    uint64_t removed = 0;
    for (const auto& [index_key, expire] : due) {
      std::string user_key = index_key.substr(9);
      std::string raw;
      std::string kv_key = std::string(1, '\x01') + user_key;
      if (db_->Get({}, kv_key, &raw).ok() && raw.size() >= 9 && (raw[0] & 1) &&
          static_cast<int64_t>(DecodeFixed64(raw.data() + 1)) == expire) {
        batch.Delete(kv_key);
        ++removed;
      }
      batch.Delete(index_key);
    }
    db_->Write({}, &batch);
    expired_.fetch_add(removed);
  }
}

}
