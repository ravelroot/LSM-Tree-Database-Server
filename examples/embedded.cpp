#include <cstdio>
#include <memory>
#include <string>

#include "arca/db.hpp"

int main(int argc, char** argv) {
  std::string path = argc > 1 ? argv[1] : "exemplo-embutido";
  arca::Options options;
  options.write_buffer_size = 1 << 20;
  std::unique_ptr<arca::DB> db;
  arca::Status s = arca::DB::Open(options, path, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "erro: %s\n", s.ToString().c_str());
    return 1;
  }

  arca::WriteBatch batch;
  batch.Put("produto:1", "Caderno universitário");
  batch.Put("produto:2", "Caneta azul");
  batch.Put("produto:3", "Mochila");
  batch.Delete("produto:2");
  db->Write(arca::WriteOptions(), &batch);

  auto snapshot = db->GetSnapshot();
  db->Put(arca::WriteOptions(), "produto:1", "Caderno de capa dura");

  std::string value;
  db->Get(arca::ReadOptions(), "produto:1", &value);
  std::printf("agora: produto:1 = %s\n", value.c_str());

  arca::ReadOptions at_snapshot;
  at_snapshot.snapshot = snapshot.get();
  db->Get(at_snapshot, "produto:1", &value);
  std::printf("no snapshot: produto:1 = %s\n", value.c_str());

  auto it = db->NewIterator(arca::ReadOptions());
  for (it->Seek("produto:"); it->Valid() && it->key().rfind("produto:", 0) == 0; it->Next()) {
    std::printf("%.*s => %.*s\n", static_cast<int>(it->key().size()), it->key().data(),
                static_cast<int>(it->value().size()), it->value().data());
  }

  db->Flush();
  auto stats = db->GetStats();
  std::printf("SSTables: %llu, sequência: %llu\n", static_cast<unsigned long long>(stats.sst_files),
              static_cast<unsigned long long>(stats.last_sequence));
  return 0;
}
