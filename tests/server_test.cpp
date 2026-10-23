#include <chrono>
#include <thread>

#include "net/resp.hpp"
#include "server/server.hpp"
#include "test.hpp"

using namespace arca;

namespace {

struct Running {
  std::string dir = test::TempDir("servidor");
  int port;
  std::unique_ptr<server::Server> srv;

  explicit Running(const std::string& password = "") {
    server::Config c;
    c.dir = dir;
    port = 20000 + static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
    c.port = port;
    c.requirepass = password;
    c.appendfsync = "no";
    c.loglevel = "warning";
    srv = std::make_unique<server::Server>(c);
    Status s = srv->Start();
    if (!s.ok()) throw test::Failure{s.ToString()};
  }

  ~Running() {
    srv.reset();
    DestroyDB(dir);
  }

  resp::Value Call(resp::Client& c, std::vector<std::string> args) {
    resp::Value v;
    Status s = c.Call(args, &v);
    if (!s.ok()) throw test::Failure{s.ToString()};
    return v;
  }
};

}

TEST(ServidorComandosBasicos) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  CHECK_EQ(r.Call(c, {"PING"}).str, std::string("PONG"));
  CHECK_EQ(r.Call(c, {"SET", "k", "v"}).str, std::string("OK"));
  CHECK_EQ(r.Call(c, {"GET", "k"}).str, std::string("v"));
  CHECK_EQ(r.Call(c, {"INCRBY", "n", "5"}).integer, int64_t(5));
  CHECK(r.Call(c, {"SET", "k", "w", "NX"}).type == resp::Value::Type::kNull);
  CHECK_EQ(r.Call(c, {"DEL", "k", "inexistente"}).integer, int64_t(1));
  CHECK(r.Call(c, {"GET", "k"}).type == resp::Value::Type::kNull);
  CHECK(r.Call(c, {"NAOEXISTE"}).type == resp::Value::Type::kError);
}

TEST(ServidorExpiracaoETransacao) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  r.Call(c, {"SET", "temp", "1", "PX", "150"});
  CHECK(r.Call(c, {"PTTL", "temp"}).integer > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(r.Call(c, {"GET", "temp"}).type == resp::Value::Type::kNull);
  r.Call(c, {"MULTI"});
  CHECK_EQ(r.Call(c, {"INCR", "x"}).str, std::string("QUEUED"));
  r.Call(c, {"INCR", "x"});
  auto exec = r.Call(c, {"EXEC"});
  CHECK_EQ(exec.elements.size(), size_t(2));
  CHECK_EQ(exec.elements[1].integer, int64_t(2));
}

TEST(ServidorSqlEPipeline) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  r.Call(c, {"SQL", "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"});
  r.Call(c, {"SQL", "INSERT INTO t VALUES (1, 'um'), (2, 'dois')"});
  auto json = r.Call(c, {"SQL.JSON", "SELECT * FROM t ORDER BY id DESC"});
  CHECK(json.str.find("\"rows\":[[2,\"dois\"],[1,\"um\"]]") != std::string::npos);
  std::string payload;
  for (int i = 0; i < 200; ++i) payload += resp::EncodeCommand({"SET", "p" + std::to_string(i), std::to_string(i)});
  CHECK_OK(c.SendRaw(payload));
  for (int i = 0; i < 200; ++i) {
    resp::Value v;
    CHECK_OK(c.Read(&v));
    CHECK_EQ(v.str, std::string("OK"));
  }
  CHECK_EQ(r.Call(c, {"GET", "p199"}).str, std::string("199"));
}

TEST(ServidorExigeSenha) {
  Running r("segredo");
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  CHECK(r.Call(c, {"GET", "x"}).type == resp::Value::Type::kError);
  CHECK(r.Call(c, {"AUTH", "errada"}).type == resp::Value::Type::kError);
  CHECK_EQ(r.Call(c, {"AUTH", "segredo"}).str, std::string("OK"));
  CHECK(r.Call(c, {"GET", "x"}).type == resp::Value::Type::kNull);
}
