#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "arca/db.hpp"
#include "net/resp.hpp"

namespace {

using arca::resp::Value;

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> Split(const std::string& line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size()) break;
    std::string arg;
    if (line[i] == '"' || line[i] == '\'') {
      char q = line[i++];
      while (i < line.size() && line[i] != q) {
        if (line[i] == '\\' && i + 1 < line.size() && q == '"') {
          char n = line[++i];
          arg.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n);
          ++i;
          continue;
        }
        arg.push_back(line[i++]);
      }
      ++i;
    } else {
      while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) arg.push_back(line[i++]);
    }
    out.push_back(arg);
  }
  return out;
}

bool LooksLikeSql(const std::string& line) {
  std::string first;
  for (char c : line) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (first.empty()) continue;
      break;
    }
    first.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  static const char* kw[] = {"select", "insert", "update", "delete", "create", "drop", "alter", "show", "describe", "explain"};
  for (const char* k : kw) {
    if (first == k) return true;
  }
  return false;
}

size_t DisplayWidth(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80 ? 1 : 0;
  return n;
}

std::string Cell(const Value& v) {
  if (v.type == Value::Type::kNull) return "NULL";
  if (v.type == Value::Type::kInteger) return std::to_string(v.integer);
  return v.str;
}

void PrintTable(const Value& reply) {
  const auto& cols = reply.elements[0].elements;
  const auto& rows = reply.elements[1].elements;
  std::vector<size_t> width(cols.size());
  for (size_t i = 0; i < cols.size(); ++i) width[i] = DisplayWidth(cols[i].str);
  for (const auto& r : rows) {
    for (size_t i = 0; i < r.elements.size() && i < width.size(); ++i) width[i] = std::max(width[i], DisplayWidth(Cell(r.elements[i])));
  }
  auto line = [&] {
    std::string s = "+";
    for (size_t w : width) s += std::string(w + 2, '-') + "+";
    std::printf("%s\n", s.c_str());
  };
  auto row = [&](const std::vector<std::string>& cells) {
    std::string s = "|";
    for (size_t i = 0; i < cells.size(); ++i) s += " " + cells[i] + std::string(width[i] - DisplayWidth(cells[i]), ' ') + " |";
    std::printf("%s\n", s.c_str());
  };
  line();
  std::vector<std::string> header;
  for (const auto& c : cols) header.push_back(c.str);
  row(header);
  line();
  for (const auto& r : rows) {
    std::vector<std::string> cells;
    for (const auto& v : r.elements) cells.push_back(Cell(v));
    row(cells);
  }
  line();
  std::printf("(%zu linha%s)\n", rows.size(), rows.size() == 1 ? "" : "s");
}

void Print(const Value& reply, bool sql) {
  if (sql && reply.type == Value::Type::kArray && reply.elements.size() == 2 &&
      reply.elements[0].type == Value::Type::kArray && reply.elements[1].type == Value::Type::kArray) {
    PrintTable(reply);
    return;
  }
  std::printf("%s\n", arca::resp::Format(reply).c_str());
}

void Usage() {
  std::printf(
      "Uso: arca-cli [-h host] [-p porta] [-a senha] [comando ...]\n\n"
      "Sem comando, abre o modo interativo. Linhas que começam com SELECT, INSERT, UPDATE,\n"
      "DELETE, CREATE, DROP, ALTER, SHOW, DESCRIBE ou EXPLAIN são enviadas como SQL.\n");
}

}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 7379;
  std::string password;
  std::vector<std::string> command;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" && i + 1 < argc) {
      host = argv[++i];
    } else if (a == "-p" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (a == "-a" && i + 1 < argc) {
      password = argv[++i];
    } else if (a == "--help") {
      Usage();
      return 0;
    } else {
      command.push_back(a);
    }
  }
  if (const char* env = std::getenv("ARCA_PASSWORD"); password.empty() && env != nullptr) password = env;

  arca::resp::Client client;
  arca::Status s = client.Connect(host, port);
  if (!s.ok()) {
    std::fprintf(stderr, "Não foi possível conectar em %s:%d: %s\n", host.c_str(), port, s.ToString().c_str());
    return 1;
  }
  if (!password.empty()) {
    Value reply;
    s = client.Call({"AUTH", password}, &reply);
    if (!s.ok() || reply.type == Value::Type::kError) {
      std::fprintf(stderr, "Falha na autenticação: %s\n", s.ok() ? reply.str.c_str() : s.ToString().c_str());
      return 1;
    }
  }

  auto run = [&](std::vector<std::string> args, bool sql) -> bool {
    Value reply;
    arca::Status st = client.Call(args, &reply);
    if (!st.ok()) {
      std::fprintf(stderr, "%s\n", st.ToString().c_str());
      return false;
    }
    Print(reply, sql);
    return reply.type != Value::Type::kError;
  };

  if (!command.empty()) {
    bool sql = Lower(command[0]) == "sql";
    return run(command, sql) ? 0 : 1;
  }

  std::printf("Arca %s em %s:%d. Digite comandos ou SQL; 'sair' encerra.\n", arca::kVersion, host.c_str(), port);
  std::string line;
  while (true) {
    std::printf("%s:%d> ", host.c_str(), port);
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    if (line.empty()) continue;
    std::string l = Lower(line);
    if (l == "sair" || l == "exit" || l == "quit") break;
    if (l == "ajuda" || l == "help") {
      Usage();
      continue;
    }
    if (LooksLikeSql(line)) {
      run({"SQL", line}, true);
    } else {
      auto args = Split(line);
      if (args.empty()) continue;
      run(args, Lower(args[0]) == "sql");
    }
    if (!client.connected()) break;
  }
  return 0;
}
