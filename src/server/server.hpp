#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "arca/db.hpp"
#include "net/resp.hpp"
#include "net/socket.hpp"
#include "server/config.hpp"
#include "sql/sql.hpp"

namespace arca::server {

using Args = std::vector<std::string>;

int64_t NowMs();

struct Session {
  bool authenticated = false;
  bool in_multi = false;
  bool multi_error = false;
  std::vector<Args> queued;
  std::shared_ptr<const Snapshot> snapshot;
};

struct Connection {
  uint64_t id = 0;
  net::socket_t fd = net::kInvalidSocket;
  int loop = 0;
  std::string addr;
  std::string in;
  size_t in_offset = 0;
  std::string out;
  size_t out_offset = 0;
  bool closing = false;
  bool closed = false;
  bool busy = false;
  bool writing = false;
  Session session;
  int64_t created_ms = 0;
  std::atomic<int64_t> last_ms{0};
  std::mutex info_mu;
  std::string name;
  std::string last_cmd;
};

class Server;

class EventLoop {
 public:
  EventLoop(Server* server, int index);
  void Start();
  void Stop();
  void Join();
  void AddConnection(net::socket_t fd, std::string peer);
  void Complete(std::shared_ptr<Connection> conn, std::string reply);
  void SetListener(net::socket_t fd) { listener_ = fd; }

 private:
  void Run();
  void HandleRead(const std::shared_ptr<Connection>& conn);
  void ProcessInput(const std::shared_ptr<Connection>& conn);
  void Flush(const std::shared_ptr<Connection>& conn);
  void Close(const std::shared_ptr<Connection>& conn);
  void AcceptAll();
  void DrainQueues();
  void CheckTimeouts();

  Server* server_;
  int index_;
  net::Poller poller_;
  net::Waker waker_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  net::socket_t listener_ = net::kInvalidSocket;
  std::mutex queue_mu_;
  std::vector<std::pair<net::socket_t, std::string>> pending_;
  std::vector<std::pair<std::shared_ptr<Connection>, std::string>> completions_;
  std::unordered_map<net::socket_t, std::shared_ptr<Connection>> conns_;
  int64_t last_timeout_check_ = 0;
};

class WorkerPool {
 public:
  explicit WorkerPool(int threads);
  ~WorkerPool();
  void Submit(std::function<void()> task);
  size_t Pending();

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> threads_;
  bool stop_ = false;
};

struct SlowlogEntry {
  int64_t id;
  int64_t at_s;
  int64_t micros;
  Args args;
  std::string addr;
  std::string name;
};

class Server {
 public:
  explicit Server(Config config);
  ~Server();

  Status Start();
  void WaitForShutdown();
  void RequestShutdown(bool save);

  enum class Dispatch { kDone, kAsync };
  Dispatch Execute(const std::shared_ptr<Connection>& conn, Args& args, std::string* out);

  void RegisterConnection(const std::shared_ptr<Connection>& conn);
  void UnregisterConnection(uint64_t id);
  uint64_t NextConnectionId() { return next_conn_id_.fetch_add(1); }
  const Config& config() const { return config_; }
  bool AcceptingMore() const { return connected_.load() < config_.maxclients; }
  void CountRejected() { rejected_.fetch_add(1); }
  void AddNetInput(size_t n) { net_in_.fetch_add(n, std::memory_order_relaxed); }
  void AddNetOutput(size_t n) { net_out_.fetch_add(n, std::memory_order_relaxed); }
  void Log(const std::string& level, const std::string& message);

  struct CommandSpec;

 private:
  friend struct Commands;

  void RunCommand(const CommandSpec& spec, Connection& conn, Args& args, std::string* out);
  void ExpireLoop();
  void StatsLoop();
  std::vector<std::unique_lock<std::mutex>> LockKeys(const std::vector<std::string>& keys);

  Config config_;
  std::mutex config_mu_;
  std::unique_ptr<DB> db_;
  std::unique_ptr<sql::Engine> sql_;
  std::vector<std::unique_ptr<EventLoop>> loops_;
  std::unique_ptr<WorkerPool> workers_;
  net::socket_t listener_ = net::kInvalidSocket;
  std::atomic<int> next_loop_{0};
  std::atomic<uint64_t> next_conn_id_{1};

  std::shared_mutex exec_mu_;
  std::array<std::mutex, 1024> stripes_;

  std::mutex registry_mu_;
  std::map<uint64_t, std::weak_ptr<Connection>> registry_;

  std::mutex slowlog_mu_;
  std::deque<SlowlogEntry> slowlog_;
  int64_t slowlog_next_id_ = 0;

  std::mutex shutdown_mu_;
  std::condition_variable shutdown_cv_;
  bool shutdown_requested_ = false;
  bool shutdown_save_ = true;

  std::thread expire_thread_;
  std::thread stats_thread_;
  std::atomic<bool> stopping_{false};

  int64_t start_ms_ = 0;
  std::atomic<int64_t> last_save_s_{0};
  std::atomic<int> connected_{0};
  std::atomic<uint64_t> total_connections_{0};
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> commands_{0};
  std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> writes_{0};
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
  std::atomic<uint64_t> expired_{0};
  std::atomic<uint64_t> net_in_{0};
  std::atomic<uint64_t> net_out_{0};
  std::atomic<double> ops_per_sec_{0};
  std::mutex log_mu_;

  friend class EventLoop;
};

}
