#pragma once

#include <cstdio>
#include <filesystem>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace arca::test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

struct Failure {
  std::string message;
};

inline std::string TempDir(const std::string& name) {
  static std::mt19937_64 rng(std::random_device{}());
  auto dir = std::filesystem::temp_directory_path() / ("arca-teste-" + name + "-" + std::to_string(rng() % 1000000));
  std::filesystem::remove_all(dir);
  return dir.string();
}

}

#define ARCA_CONCAT_INNER(a, b) a##b
#define ARCA_CONCAT(a, b) ARCA_CONCAT_INNER(a, b)

#define TEST(name)                                                                              \
  static void ARCA_CONCAT(test_, name)();                                                       \
  static ::arca::test::Registrar ARCA_CONCAT(registrar_, name)(#name, ARCA_CONCAT(test_, name)); \
  static void ARCA_CONCAT(test_, name)()

#define CHECK(cond)                                                                                 \
  do {                                                                                              \
    if (!(cond)) {                                                                                  \
      std::ostringstream arca_os;                                                                   \
      arca_os << __FILE__ << ":" << __LINE__ << ": falhou " << #cond;                               \
      throw ::arca::test::Failure{arca_os.str()};                                                   \
    }                                                                                               \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto arca_va = (a);                                                                             \
    auto arca_vb = (b);                                                                             \
    if (!(arca_va == arca_vb)) {                                                                    \
      std::ostringstream arca_os;                                                                   \
      arca_os << __FILE__ << ":" << __LINE__ << ": " << #a << " = [" << arca_va << "] esperado ["   \
              << arca_vb << "]";                                                                    \
      throw ::arca::test::Failure{arca_os.str()};                                                   \
    }                                                                                               \
  } while (0)

#define CHECK_OK(expr)                                                                              \
  do {                                                                                              \
    auto arca_st = (expr);                                                                          \
    if (!arca_st.ok()) {                                                                            \
      std::ostringstream arca_os;                                                                   \
      arca_os << __FILE__ << ":" << __LINE__ << ": " << #expr << " -> " << arca_st.ToString();      \
      throw ::arca::test::Failure{arca_os.str()};                                                   \
    }                                                                                               \
  } while (0)
