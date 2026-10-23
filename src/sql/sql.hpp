#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "arca/db.hpp"

namespace arca::sql {

enum class Type { kNull, kInt, kReal, kText, kBool };

const char* TypeName(Type t);

struct Value {
  Type type = Type::kNull;
  int64_t i = 0;
  double d = 0;
  bool b = false;
  std::string s;

  static Value Null() { return Value(); }
  static Value Int(int64_t v) {
    Value x;
    x.type = Type::kInt;
    x.i = v;
    return x;
  }
  static Value Real(double v) {
    Value x;
    x.type = Type::kReal;
    x.d = v;
    return x;
  }
  static Value Text(std::string v) {
    Value x;
    x.type = Type::kText;
    x.s = std::move(v);
    return x;
  }
  static Value Bool(bool v) {
    Value x;
    x.type = Type::kBool;
    x.b = v;
    return x;
  }
  bool is_null() const { return type == Type::kNull; }
  std::string ToString() const;
  std::string ToJson() const;
};

int Compare(const Value& a, const Value& b);
bool Truthy(const Value& v);

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  enum class Kind { kLiteral, kColumn, kUnary, kBinary, kFunction, kIsNull, kIn, kBetween, kLike, kStar };
  Kind kind = Kind::kLiteral;
  Value literal;
  std::string name;
  std::string op;
  bool negated = false;
  bool distinct = false;
  std::vector<ExprPtr> args;
  int column_index = -1;
};

struct ColumnDef {
  std::string name;
  Type type = Type::kText;
  bool primary_key = false;
  bool not_null = false;
  bool auto_increment = false;
  bool has_default = false;
  Value default_value;
};

struct IndexDef {
  std::string name;
  uint32_t id = 0;
  int column = 0;
};

struct TableDef {
  std::string name;
  uint32_t id = 0;
  std::vector<ColumnDef> columns;
  int pk = 0;
  std::vector<IndexDef> indexes;
  int FindColumn(std::string_view name) const;
};

struct SelectItem {
  ExprPtr expr;
  std::string alias;
};

struct OrderItem {
  ExprPtr expr;
  bool desc = false;
};

struct Statement {
  enum class Kind {
    kCreateTable,
    kDropTable,
    kAlterAddColumn,
    kCreateIndex,
    kDropIndex,
    kInsert,
    kSelect,
    kUpdate,
    kDelete,
    kShowTables,
    kDescribe
  };
  Kind kind = Kind::kSelect;
  bool explain = false;
  bool if_not_exists = false;
  bool if_exists = false;
  std::string table;
  std::string index;
  std::string column;
  std::vector<ColumnDef> columns;
  std::vector<std::string> insert_columns;
  std::vector<std::vector<ExprPtr>> values;
  std::vector<SelectItem> select;
  ExprPtr where;
  std::vector<ExprPtr> group_by;
  std::vector<OrderItem> order_by;
  int64_t limit = -1;
  int64_t offset = 0;
  std::vector<std::pair<std::string, ExprPtr>> assignments;
};

Status Parse(std::string_view text, Statement* out);

struct Result {
  bool has_rows = false;
  std::vector<std::string> columns;
  std::vector<std::string> types;
  std::vector<std::vector<Value>> rows;
  std::string tag;
  int64_t affected = 0;
  std::string plan;
  double ms = 0;
};

struct TableSummary {
  TableDef def;
  int64_t rows = 0;
};

class Engine {
 public:
  explicit Engine(DB* db);
  Status Load();
  Status Execute(std::string_view text, Result* result, const Snapshot* snapshot = nullptr);
  std::vector<TableSummary> Tables();
  size_t TableCount();
  static std::string DescribeKey(std::string_view key, const std::map<uint32_t, std::string>& names);
  std::map<uint32_t, std::string> TableNames();

 private:
  struct TableState {
    TableDef def;
    std::shared_mutex mu;
  };

  Status ExecSelect(Statement& st, Result* r, const Snapshot* snap);
  Status ExecInsert(Statement& st, Result* r);
  Status ExecUpdate(Statement& st, Result* r);
  Status ExecDelete(Statement& st, Result* r);
  Status ExecCreateTable(Statement& st, Result* r);
  Status ExecDropTable(Statement& st, Result* r);
  Status ExecAlterAddColumn(Statement& st, Result* r);
  Status ExecCreateIndex(Statement& st, Result* r);
  Status ExecDropIndex(Statement& st, Result* r);
  Status ExecShowTables(Result* r);
  Status ExecDescribe(Statement& st, Result* r);

  std::shared_ptr<TableState> FindTable(const std::string& name);
  Status SaveTable(const TableDef& def, WriteBatch* batch);
  uint32_t NextId();
  int64_t ReadCounter(const std::string& key);
  void WriteCounter(WriteBatch* batch, const std::string& key, int64_t value);
  Status ScanRows(const TableDef& def, const Expr* where, const Snapshot* snap,
                  std::vector<std::vector<Value>>* rows, std::string* plan);

  DB* db_;
  std::shared_mutex catalog_mu_;
  std::mutex id_mu_;
  std::map<std::string, std::shared_ptr<TableState>> tables_;
};

std::string Lower(std::string_view s);
std::string EncodeRowKey(uint32_t table_id, const Value& pk);
std::string EncodeRow(const std::vector<Value>& row);
bool DecodeRow(std::string_view data, size_t columns, std::vector<Value>* row);

}
