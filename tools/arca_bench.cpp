#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/resp.hpp"

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 7379;
  int clients = 8;
  int requests = 100000;
  int pipeline = 16;
  int value_size = 64;
  int keyspace = 100000;
  std::string password;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    std::string v = argv[i + 1];
    if (a == "-h") host = v;
    if (a == "-p") port = std::atoi(v.c_str());
    if (a == "-c") clients = std::atoi(v.c_str());
    if (a == "-n") requests = std::atoi(v.c_str());
    if (a == "-P") pipeline = std::atoi(v.c_str());
    if (a == "-d") value_size = std::atoi(v.c_str());
    if (a == "-r") keyspace = std::atoi(v.c_str());
    if (a == "-a") password = v;
  }
  std::printf("arca-bench: %d clientes, %d requisições por teste, pipeline %d, valores de %d bytes\n\n", clients,
              requests, pipeline, value_size);

  for (const char* test : {"SET", "GET"}) {
    std::atomic<int> remaining{requests};
    std::mutex mu;
    std::vector<double> latencies;
    std::atomic<int> errors{0};
    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int c = 0; c < clients; ++c) {
      threads.emplace_back([&, c] {
        arca::resp::Client client;
        if (!client.Connect(host, port).ok()) {
          ++errors;
          return;
        }
        if (!password.empty()) {
          arca::resp::Value r;
          client.Call({"AUTH", password}, &r);
        }
        std::mt19937 rng(static_cast<unsigned>(c * 7919 + 1));
        std::string value(static_cast<size_t>(value_size), 'x');
        std::vector<double> local;
        while (true) {
          int batch = std::min(pipeline, remaining.fetch_sub(pipeline));
          if (batch <= 0) break;
          std::string payload;
          for (int i = 0; i < batch; ++i) {
            std::string key = "bench:" + std::to_string(rng() % static_cast<unsigned>(keyspace));
            payload += std::string(test) == "SET" ? arca::resp::EncodeCommand({"SET", key, value})
                                                  : arca::resp::EncodeCommand({"GET", key});
          }
          auto t0 = std::chrono::steady_clock::now();
          if (!client.SendRaw(payload).ok()) {
            ++errors;
            return;
          }
          for (int i = 0; i < batch; ++i) {
            arca::resp::Value r;
            if (!client.Read(&r).ok() || r.type == arca::resp::Value::Type::kError) ++errors;
          }
          double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
          local.push_back(us / batch);
        }
        std::lock_guard<std::mutex> lock(mu);
        latencies.insert(latencies.end(), local.begin(), local.end());
      });
    }
    for (auto& t : threads) t.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::sort(latencies.begin(), latencies.end());
    auto pct = [&](double p) { return latencies.empty() ? 0.0 : latencies[static_cast<size_t>(p * (latencies.size() - 1))]; };
    std::printf("%-4s %10.0f ops/s   p50 %7.1f us   p99 %7.1f us   erros %d\n", test, requests / secs, pct(0.50),
                pct(0.99), errors.load());
  }
  return 0;
}
