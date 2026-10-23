#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "arca/db.hpp"
#include "server/server.hpp"

namespace {

std::atomic<bool> g_signal{false};

void OnSignal(int) { g_signal.store(true); }

void PrintHelp() {
  std::printf(
      "Arca %s, banco de dados com LSM-Tree\n\n"
      "Uso: arca-server [arquivo.conf] [--opção valor ...]\n\n"
      "  --port 7379              porta TCP\n"
      "  --bind 127.0.0.1         endereço de escuta (0.0.0.0 para todas as interfaces)\n"
      "  --dir dados              pasta dos arquivos do banco\n"
      "  --requirepass senha      exige AUTH dos clientes\n"
      "  --io-threads 2           threads de rede\n"
      "  --worker-threads 4       threads para comandos pesados (SQL, compactação)\n"
      "  --appendfsync everysec   always, everysec ou no\n"
      "  --write-buffer-size 4mb  tamanho da memtable\n"
      "  --block-cache-size 64mb  cache de blocos das SSTables\n"
      "  --version                mostra a versão\n\n"
      "Qualquer parâmetro do arquivo de configuração também vale como --nome valor.\n",
      arca::kVersion);
}

}

int main(int argc, char** argv) {
  arca::server::Config config;
  int i = 1;
  if (i < argc && argv[i][0] != '-') {
    arca::Status s = config.LoadFile(argv[i]);
    if (!s.ok()) {
      std::fprintf(stderr, "erro: %s\n", s.ToString().c_str());
      return 1;
    }
    ++i;
  }
  config.ApplyEnvironment();
  for (; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintHelp();
      return 0;
    }
    if (arg == "--version" || arg == "-v") {
      std::printf("arca-server %s\n", arca::kVersion);
      return 0;
    }
    if (arg.rfind("--", 0) != 0 || i + 1 >= argc) {
      std::fprintf(stderr, "erro: argumento inválido '%s' (use --help)\n", arg.c_str());
      return 1;
    }
    arca::Status s = config.Set(arg.substr(2), argv[++i]);
    if (!s.ok()) {
      std::fprintf(stderr, "erro: %s\n", s.ToString().c_str());
      return 1;
    }
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif

  auto server = std::make_unique<arca::server::Server>(config);
  arca::Status s = server->Start();
  if (!s.ok()) {
    std::fprintf(stderr, "erro ao iniciar: %s\n", s.ToString().c_str());
    return 1;
  }
  std::thread watcher([&server] {
    while (!g_signal.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server->RequestShutdown(true);
  });
  server->WaitForShutdown();
  g_signal.store(true);
  watcher.join();
  server->Log("info", "desligando: gravando a memtable e fechando os arquivos");
  server.reset();
  std::fprintf(stderr, "Arca encerrada.\n");
  return 0;
}
