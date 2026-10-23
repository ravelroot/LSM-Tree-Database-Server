#include <chrono>
#include <cstdio>
#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int passed = 0;
  int failed = 0;
  for (const auto& c : arca::test::Registry()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
    auto start = std::chrono::steady_clock::now();
    try {
      c.fn();
      ++passed;
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      std::printf("[ ok ] %s (%.0f ms)\n", c.name, ms);
    } catch (const arca::test::Failure& f) {
      ++failed;
      std::printf("[FALHA] %s\n       %s\n", c.name, f.message.c_str());
    } catch (const std::exception& e) {
      ++failed;
      std::printf("[FALHA] %s\n       exceção: %s\n", c.name, e.what());
    }
    std::fflush(stdout);
  }
  std::printf("\n%d teste(s) passaram, %d falharam\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
