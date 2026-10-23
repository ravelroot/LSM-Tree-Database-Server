#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "db/dbformat.hpp"
#include "db/filename.hpp"
#include "db/log.hpp"
#include "db/version_edit.hpp"
#include "db/write_batch_internal.hpp"
#include "table/table.hpp"
#include "util/file.hpp"

using namespace arca;

namespace {

std::string Printable(std::string_view s, size_t max = 60) {
  std::string out;
  for (unsigned char c : s.substr(0, max)) {
    if (c >= 32 && c < 127 && c != '\\') {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  if (s.size() > max) out += "...";
  return out;
}

class PrintReporter final : public wal::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& s) override {
    std::printf("  ! %zu bytes descartados: %s\n", bytes, s.ToString().c_str());
  }
};

int DumpSst(const std::string& path, bool all) {
  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(path, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  uint64_t size = file->size();
  std::shared_ptr<Table> table;
  s = Table::Open(Options(), std::move(file), nullptr, nullptr, &table);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  const auto& p = table->properties();
  std::printf("SSTable %s\n", path.c_str());
  std::printf("  tamanho do arquivo : %llu bytes\n", static_cast<unsigned long long>(size));
  std::printf("  entradas           : %llu (%llu remoções)\n", static_cast<unsigned long long>(p.entries),
              static_cast<unsigned long long>(p.deletions));
  std::printf("  blocos de dados    : %llu\n", static_cast<unsigned long long>(p.data_blocks));
  std::printf("  bytes de chaves    : %llu\n", static_cast<unsigned long long>(p.raw_key_bytes));
  std::printf("  bytes de valores   : %llu\n", static_cast<unsigned long long>(p.raw_value_bytes));
  std::printf("  sequências         : %llu .. %llu\n", static_cast<unsigned long long>(p.smallest_seq),
              static_cast<unsigned long long>(p.largest_seq));
  std::printf("  bloom (bits/chave) : %llu\n", static_cast<unsigned long long>(p.bloom_bits_per_key));
  s = table->VerifyAll();
  std::printf("  verificação        : %s\n", s.ok() ? "checksums e ordem corretos" : s.ToString().c_str());
  if (all) {
    auto it = table->NewIterator(ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ParsedInternalKey k;
      ParseInternalKey(it->key(), &k);
      std::printf("  %s @%llu %s => %s\n", Printable(k.user_key).c_str(), static_cast<unsigned long long>(k.sequence),
                  k.type == kTypeValue ? "valor" : "remoção", Printable(it->value()).c_str());
    }
  }
  return s.ok() ? 0 : 2;
}

int DumpLog(const std::string& path, bool manifest) {
  std::unique_ptr<SequentialFile> file;
  Status s = SequentialFile::Open(path, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  PrintReporter reporter;
  wal::Reader reader(file.get(), &reporter);
  std::string scratch;
  std::string_view record;
  int count = 0;
  while (reader.ReadRecord(&record, &scratch)) {
    ++count;
    if (manifest) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      std::printf("#%d %s\n", count, s.ok() ? edit.DebugString().c_str() : s.ToString().c_str());
      continue;
    }
    WriteBatch batch;
    if (record.size() < WriteBatchInternal::kHeader) {
      std::printf("#%d registro curto demais\n", count);
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);
    std::printf("#%d sequência %llu, %u operação(ões)\n", count,
                static_cast<unsigned long long>(WriteBatchInternal::Sequence(&batch)), WriteBatchInternal::Count(&batch));
    struct Printer final : WriteBatch::Handler {
      void Put(std::string_view k, std::string_view v) override {
        std::printf("    PUT %s => %s\n", Printable(k).c_str(), Printable(v).c_str());
      }
      void Delete(std::string_view k) override { std::printf("    DEL %s\n", Printable(k).c_str()); }
    } printer;
    batch.Iterate(&printer);
  }
  std::printf("%d registro(s)\n", count);
  return 0;
}

int CheckDir(const std::string& dir) {
  std::vector<std::string> names;
  Status s = GetChildren(dir, &names);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.ToString().c_str());
    return 1;
  }
  int bad = 0;
  int tables = 0;
  for (const auto& n : names) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(n, &number, &type) || type != FileType::kTable) continue;
    ++tables;
    std::unique_ptr<RandomAccessFile> file;
    std::shared_ptr<Table> table;
    s = RandomAccessFile::Open(JoinPath(dir, n), &file);
    if (s.ok()) s = Table::Open(Options(), std::move(file), nullptr, nullptr, &table);
    if (s.ok()) s = table->VerifyAll();
    std::printf("%-14s %s\n", n.c_str(), s.ok() ? "ok" : s.ToString().c_str());
    if (!s.ok()) ++bad;
  }
  std::printf("\n%d SSTable(s) verificadas, %d com problema\n", tables, bad);
  return bad == 0 ? 0 : 2;
}

}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf(
        "Uso:\n"
        "  arca-inspect sst <arquivo.sst> [--tudo]   propriedades, verificação e conteúdo de uma SSTable\n"
        "  arca-inspect wal <arquivo.wal>            lotes gravados no log de escrita\n"
        "  arca-inspect manifest <MANIFEST-n>        histórico de versões (arquivos por nível)\n"
        "  arca-inspect check <pasta>                verifica os checksums de todas as SSTables\n");
    return 1;
  }
  std::string cmd = argv[1];
  if (cmd == "sst") return DumpSst(argv[2], argc > 3 && std::strcmp(argv[3], "--tudo") == 0);
  if (cmd == "wal") return DumpLog(argv[2], false);
  if (cmd == "manifest") return DumpLog(argv[2], true);
  if (cmd == "check") return CheckDir(argv[2]);
  std::fprintf(stderr, "subcomando desconhecido: %s\n", cmd.c_str());
  return 1;
}
