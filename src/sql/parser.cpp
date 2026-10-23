#include <cctype>
#include <charconv>
#include <cstdlib>

#include "sql/sql.hpp"

namespace arca::sql {

std::string Lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

namespace {

struct Token {
  enum class Kind { kIdent, kQuotedIdent, kString, kNumber, kSymbol, kEnd };
  Kind kind = Kind::kEnd;
  std::string text;
  size_t pos = 0;
};

Status Tokenize(std::string_view in, std::vector<Token>* out) {
  size_t i = 0;
  while (i < in.size()) {
    char c = in[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < in.size() && in[i + 1] == '-') {
      while (i < in.size() && in[i] != '\n') ++i;
      continue;
    }
    Token t;
    t.pos = i;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
      size_t start = i;
      while (i < in.size() && (std::isalnum(static_cast<unsigned char>(in[i])) || in[i] == '_' ||
                               static_cast<unsigned char>(in[i]) >= 0x80)) {
        ++i;
      }
      t.kind = Token::Kind::kIdent;
      t.text = std::string(in.substr(start, i - start));
    } else if (std::isdigit(static_cast<unsigned char>(c)) ||
               (c == '.' && i + 1 < in.size() && std::isdigit(static_cast<unsigned char>(in[i + 1])))) {
      size_t start = i;
      while (i < in.size() && (std::isdigit(static_cast<unsigned char>(in[i])) || in[i] == '.')) ++i;
      if (i < in.size() && (in[i] == 'e' || in[i] == 'E')) {
        ++i;
        if (i < in.size() && (in[i] == '+' || in[i] == '-')) ++i;
        while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i]))) ++i;
      }
      t.kind = Token::Kind::kNumber;
      t.text = std::string(in.substr(start, i - start));
    } else if (c == '\'') {
      ++i;
      std::string s;
      bool closed = false;
      while (i < in.size()) {
        if (in[i] == '\'') {
          if (i + 1 < in.size() && in[i + 1] == '\'') {
            s.push_back('\'');
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        s.push_back(in[i++]);
      }
      if (!closed) return Status::InvalidArgument("texto entre aspas simples não foi fechado");
      t.kind = Token::Kind::kString;
      t.text = std::move(s);
    } else if (c == '"' || c == '`') {
      char quote = c;
      ++i;
      size_t start = i;
      while (i < in.size() && in[i] != quote) ++i;
      if (i >= in.size()) return Status::InvalidArgument("identificador entre aspas não foi fechado");
      t.kind = Token::Kind::kQuotedIdent;
      t.text = std::string(in.substr(start, i - start));
      ++i;
    } else {
      static const char* two[] = {"<=", ">=", "!=", "<>", "||"};
      t.kind = Token::Kind::kSymbol;
      bool matched = false;
      for (const char* op : two) {
        if (in.substr(i, 2) == op) {
          t.text = op;
          i += 2;
          matched = true;
          break;
        }
      }
      if (!matched) {
        if (std::string_view("=<>+-*/%(),;.").find(c) == std::string_view::npos) {
          return Status::InvalidArgument(std::string("caractere inesperado '") + c + "'");
        }
        t.text = std::string(1, c);
        ++i;
      }
    }
    out->push_back(std::move(t));
  }
  Token end;
  end.pos = in.size();
  out->push_back(end);
  return Status::OK();
}

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Status ParseStatement(Statement* st) {
    if (AcceptKeyword("explain")) st->explain = true;
    if (AcceptKeyword("create")) {
      if (AcceptKeyword("table")) return ParseCreateTable(st);
      if (AcceptKeyword("index")) return ParseCreateIndex(st);
      return Error("esperado TABLE ou INDEX depois de CREATE");
    }
    if (AcceptKeyword("drop")) {
      if (AcceptKeyword("table")) {
        st->kind = Statement::Kind::kDropTable;
        if (AcceptKeyword("if")) {
          if (!ExpectKeyword("exists")) return status_;
          st->if_exists = true;
        }
        if (!ParseName(&st->table)) return status_;
        return Finish();
      }
      if (AcceptKeyword("index")) {
        st->kind = Statement::Kind::kDropIndex;
        if (AcceptKeyword("if")) {
          if (!ExpectKeyword("exists")) return status_;
          st->if_exists = true;
        }
        if (!ParseName(&st->index)) return status_;
        return Finish();
      }
      return Error("esperado TABLE ou INDEX depois de DROP");
    }
    if (AcceptKeyword("alter")) {
      if (!ExpectKeyword("table")) return status_;
      st->kind = Statement::Kind::kAlterAddColumn;
      if (!ParseName(&st->table)) return status_;
      if (!ExpectKeyword("add")) return status_;
      AcceptKeyword("column");
      ColumnDef col;
      if (!ParseColumnDef(&col)) return status_;
      st->columns.push_back(std::move(col));
      return Finish();
    }
    if (AcceptKeyword("insert")) return ParseInsert(st);
    if (AcceptKeyword("select")) return ParseSelect(st);
    if (AcceptKeyword("update")) return ParseUpdate(st);
    if (AcceptKeyword("delete")) return ParseDelete(st);
    if (AcceptKeyword("show")) {
      if (!ExpectKeyword("tables")) return status_;
      st->kind = Statement::Kind::kShowTables;
      return Finish();
    }
    if (AcceptKeyword("describe") || AcceptKeyword("desc")) {
      st->kind = Statement::Kind::kDescribe;
      if (!ParseName(&st->table)) return status_;
      return Finish();
    }
    return Error("comando SQL não reconhecido");
  }

 private:
  const Token& Peek(size_t ahead = 0) const { return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)]; }
  void Advance() {
    if (pos_ < tokens_.size() - 1) ++pos_;
  }

  bool IsKeyword(const Token& t, const char* kw) const { return t.kind == Token::Kind::kIdent && Lower(t.text) == kw; }

  bool AcceptKeyword(const char* kw) {
    if (IsKeyword(Peek(), kw)) {
      Advance();
      return true;
    }
    return false;
  }

  bool ExpectKeyword(const char* kw) {
    if (AcceptKeyword(kw)) return true;
    std::string up(kw);
    for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    Error("esperado " + up + " perto de '" + Peek().text + "'");
    return false;
  }

  bool AcceptSymbol(const char* s) {
    if (Peek().kind == Token::Kind::kSymbol && Peek().text == s) {
      Advance();
      return true;
    }
    return false;
  }

  bool ExpectSymbol(const char* s) {
    if (AcceptSymbol(s)) return true;
    Error(std::string("esperado '") + s + "' perto de '" + Peek().text + "'");
    return false;
  }

  Status Error(const std::string& msg) {
    if (status_.ok()) status_ = Status::InvalidArgument(msg);
    return status_;
  }

  Status Finish() {
    AcceptSymbol(";");
    if (!status_.ok()) return status_;
    if (Peek().kind != Token::Kind::kEnd) return Error("texto inesperado perto de '" + Peek().text + "'");
    return Status::OK();
  }

  bool ParseName(std::string* out) {
    const Token& t = Peek();
    if (t.kind == Token::Kind::kIdent || t.kind == Token::Kind::kQuotedIdent) {
      *out = t.kind == Token::Kind::kIdent ? Lower(t.text) : t.text;
      Advance();
      return true;
    }
    Error("esperado um nome perto de '" + t.text + "'");
    return false;
  }

  bool ParseType(Type* type) {
    std::string name;
    if (!ParseName(&name)) return false;
    if (name == "int" || name == "integer" || name == "bigint" || name == "smallint" || name == "timestamp") {
      *type = Type::kInt;
    } else if (name == "real" || name == "float" || name == "double" || name == "numeric" || name == "decimal") {
      *type = Type::kReal;
    } else if (name == "text" || name == "varchar" || name == "string" || name == "char") {
      *type = Type::kText;
    } else if (name == "bool" || name == "boolean") {
      *type = Type::kBool;
    } else {
      Error("tipo desconhecido '" + name + "' (use INT, REAL, TEXT ou BOOL)");
      return false;
    }
    if (AcceptSymbol("(")) {
      while (!AcceptSymbol(")")) {
        if (Peek().kind == Token::Kind::kEnd) {
          Error("parêntese do tipo não foi fechado");
          return false;
        }
        Advance();
      }
    }
    if (name == "double") AcceptKeyword("precision");
    return true;
  }

  bool ParseLiteral(Value* v) {
    bool negative = AcceptSymbol("-");
    const Token& t = Peek();
    if (t.kind == Token::Kind::kNumber) {
      *v = NumberValue(t.text, negative);
      Advance();
      return true;
    }
    if (negative) {
      Error("esperado número depois do sinal de menos");
      return false;
    }
    if (t.kind == Token::Kind::kString) {
      *v = Value::Text(t.text);
      Advance();
      return true;
    }
    if (IsKeyword(t, "true") || IsKeyword(t, "false")) {
      *v = Value::Bool(IsKeyword(t, "true"));
      Advance();
      return true;
    }
    if (IsKeyword(t, "null")) {
      *v = Value::Null();
      Advance();
      return true;
    }
    Error("esperado um valor literal perto de '" + t.text + "'");
    return false;
  }

  static Value NumberValue(const std::string& text, bool negative) {
    if (text.find_first_of(".eE") == std::string::npos) {
      int64_t v = 0;
      auto r = std::from_chars(text.data(), text.data() + text.size(), v);
      if (r.ec == std::errc()) return Value::Int(negative ? -v : v);
    }
    double d = std::strtod(text.c_str(), nullptr);
    return Value::Real(negative ? -d : d);
  }

  bool ParseColumnDef(ColumnDef* col) {
    if (!ParseName(&col->name)) return false;
    if (!ParseType(&col->type)) return false;
    while (true) {
      if (AcceptKeyword("primary")) {
        if (!ExpectKeyword("key")) return false;
        col->primary_key = true;
        col->not_null = true;
      } else if (AcceptKeyword("not")) {
        if (!ExpectKeyword("null")) return false;
        col->not_null = true;
      } else if (AcceptKeyword("null")) {
      } else if (AcceptKeyword("unique")) {
      } else if (AcceptKeyword("autoincrement") || AcceptKeyword("auto_increment")) {
        col->auto_increment = true;
      } else if (AcceptKeyword("default")) {
        if (!ParseLiteral(&col->default_value)) return false;
        col->has_default = true;
      } else {
        return true;
      }
    }
  }

  Status ParseCreateTable(Statement* st) {
    st->kind = Statement::Kind::kCreateTable;
    if (AcceptKeyword("if")) {
      if (!ExpectKeyword("not") || !ExpectKeyword("exists")) return status_;
      st->if_not_exists = true;
    }
    if (!ParseName(&st->table) || !ExpectSymbol("(")) return status_;
    do {
      if (AcceptKeyword("primary")) {
        if (!ExpectKeyword("key") || !ExpectSymbol("(")) return status_;
        std::string pk;
        if (!ParseName(&pk) || !ExpectSymbol(")")) return status_;
        bool found = false;
        for (auto& c : st->columns) {
          if (c.name == pk) {
            c.primary_key = true;
            c.not_null = true;
            found = true;
          }
        }
        if (!found) return Error("coluna da chave primária não existe: " + pk);
        continue;
      }
      ColumnDef col;
      if (!ParseColumnDef(&col)) return status_;
      st->columns.push_back(std::move(col));
    } while (AcceptSymbol(","));
    if (!ExpectSymbol(")")) return status_;
    return Finish();
  }

  Status ParseCreateIndex(Statement* st) {
    st->kind = Statement::Kind::kCreateIndex;
    if (AcceptKeyword("if")) {
      if (!ExpectKeyword("not") || !ExpectKeyword("exists")) return status_;
      st->if_not_exists = true;
    }
    if (!ParseName(&st->index) || !ExpectKeyword("on") || !ParseName(&st->table) || !ExpectSymbol("(") ||
        !ParseName(&st->column) || !ExpectSymbol(")")) {
      return status_;
    }
    return Finish();
  }

  Status ParseInsert(Statement* st) {
    st->kind = Statement::Kind::kInsert;
    if (!ExpectKeyword("into") || !ParseName(&st->table)) return status_;
    if (AcceptSymbol("(")) {
      do {
        std::string c;
        if (!ParseName(&c)) return status_;
        st->insert_columns.push_back(std::move(c));
      } while (AcceptSymbol(","));
      if (!ExpectSymbol(")")) return status_;
    }
    if (!ExpectKeyword("values")) return status_;
    do {
      if (!ExpectSymbol("(")) return status_;
      std::vector<ExprPtr> row;
      do {
        ExprPtr e = ParseExpr();
        if (!e) return status_;
        row.push_back(std::move(e));
      } while (AcceptSymbol(","));
      if (!ExpectSymbol(")")) return status_;
      st->values.push_back(std::move(row));
    } while (AcceptSymbol(","));
    return Finish();
  }

  Status ParseSelect(Statement* st) {
    st->kind = Statement::Kind::kSelect;
    do {
      SelectItem item;
      if (AcceptSymbol("*")) {
        item.expr = std::make_unique<Expr>();
        item.expr->kind = Expr::Kind::kStar;
      } else {
        item.expr = ParseExpr();
        if (!item.expr) return status_;
        if (AcceptKeyword("as")) {
          if (!ParseName(&item.alias)) return status_;
        } else if ((Peek().kind == Token::Kind::kIdent && !IsKeyword(Peek(), "from")) ||
                   Peek().kind == Token::Kind::kQuotedIdent) {
          ParseName(&item.alias);
        }
      }
      st->select.push_back(std::move(item));
    } while (AcceptSymbol(","));
    if (!ExpectKeyword("from") || !ParseName(&st->table)) return status_;
    if (AcceptKeyword("where")) {
      st->where = ParseExpr();
      if (!st->where) return status_;
    }
    if (AcceptKeyword("group")) {
      if (!ExpectKeyword("by")) return status_;
      do {
        ExprPtr e = ParseExpr();
        if (!e) return status_;
        st->group_by.push_back(std::move(e));
      } while (AcceptSymbol(","));
    }
    if (AcceptKeyword("order")) {
      if (!ExpectKeyword("by")) return status_;
      do {
        OrderItem o;
        o.expr = ParseExpr();
        if (!o.expr) return status_;
        if (AcceptKeyword("desc")) {
          o.desc = true;
        } else {
          AcceptKeyword("asc");
        }
        st->order_by.push_back(std::move(o));
      } while (AcceptSymbol(","));
    }
    if (AcceptKeyword("limit")) {
      Value v;
      if (!ParseLiteral(&v) || v.type != Type::kInt || v.i < 0) return Error("LIMIT precisa de um inteiro");
      st->limit = v.i;
      if (AcceptKeyword("offset")) {
        if (!ParseLiteral(&v) || v.type != Type::kInt || v.i < 0) return Error("OFFSET precisa de um inteiro");
        st->offset = v.i;
      }
    }
    return Finish();
  }

  Status ParseUpdate(Statement* st) {
    st->kind = Statement::Kind::kUpdate;
    if (!ParseName(&st->table) || !ExpectKeyword("set")) return status_;
    do {
      std::string col;
      if (!ParseName(&col) || !ExpectSymbol("=")) return status_;
      ExprPtr e = ParseExpr();
      if (!e) return status_;
      st->assignments.emplace_back(std::move(col), std::move(e));
    } while (AcceptSymbol(","));
    if (AcceptKeyword("where")) {
      st->where = ParseExpr();
      if (!st->where) return status_;
    }
    return Finish();
  }

  Status ParseDelete(Statement* st) {
    st->kind = Statement::Kind::kDelete;
    if (!ExpectKeyword("from") || !ParseName(&st->table)) return status_;
    if (AcceptKeyword("where")) {
      st->where = ParseExpr();
      if (!st->where) return status_;
    }
    return Finish();
  }

  static ExprPtr MakeBinary(std::string op, ExprPtr a, ExprPtr b) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::kBinary;
    e->op = std::move(op);
    e->args.push_back(std::move(a));
    e->args.push_back(std::move(b));
    return e;
  }

  ExprPtr ParseExpr() { return ParseOr(); }

  ExprPtr ParseOr() {
    ExprPtr left = ParseAnd();
    while (left && AcceptKeyword("or")) {
      ExprPtr right = ParseAnd();
      if (!right) return nullptr;
      left = MakeBinary("or", std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr ParseAnd() {
    ExprPtr left = ParseNot();
    while (left && AcceptKeyword("and")) {
      ExprPtr right = ParseNot();
      if (!right) return nullptr;
      left = MakeBinary("and", std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr ParseNot() {
    if (AcceptKeyword("not")) {
      ExprPtr inner = ParseNot();
      if (!inner) return nullptr;
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kUnary;
      e->op = "not";
      e->args.push_back(std::move(inner));
      return e;
    }
    return ParseComparison();
  }

  ExprPtr ParseComparison() {
    ExprPtr left = ParseAdditive();
    if (!left) return nullptr;
    static const char* ops[] = {"=", "!=", "<>", "<=", ">=", "<", ">"};
    for (const char* op : ops) {
      if (AcceptSymbol(op)) {
        ExprPtr right = ParseAdditive();
        if (!right) return nullptr;
        return MakeBinary(std::string(op) == "<>" ? "!=" : op, std::move(left), std::move(right));
      }
    }
    if (AcceptKeyword("is")) {
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kIsNull;
      e->negated = AcceptKeyword("not");
      if (!ExpectKeyword("null")) return nullptr;
      e->args.push_back(std::move(left));
      return e;
    }
    bool negated = false;
    if (IsKeyword(Peek(), "not") &&
        (IsKeyword(Peek(1), "in") || IsKeyword(Peek(1), "between") || IsKeyword(Peek(1), "like"))) {
      Advance();
      negated = true;
    }
    if (AcceptKeyword("in")) {
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kIn;
      e->negated = negated;
      e->args.push_back(std::move(left));
      if (!ExpectSymbol("(")) return nullptr;
      do {
        ExprPtr item = ParseExpr();
        if (!item) return nullptr;
        e->args.push_back(std::move(item));
      } while (AcceptSymbol(","));
      if (!ExpectSymbol(")")) return nullptr;
      return e;
    }
    if (AcceptKeyword("between")) {
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kBetween;
      e->negated = negated;
      e->args.push_back(std::move(left));
      ExprPtr lo = ParseAdditive();
      if (!lo || !ExpectKeyword("and")) return nullptr;
      ExprPtr hi = ParseAdditive();
      if (!hi) return nullptr;
      e->args.push_back(std::move(lo));
      e->args.push_back(std::move(hi));
      return e;
    }
    if (AcceptKeyword("like")) {
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kLike;
      e->negated = negated;
      e->args.push_back(std::move(left));
      ExprPtr pattern = ParseAdditive();
      if (!pattern) return nullptr;
      e->args.push_back(std::move(pattern));
      return e;
    }
    if (negated) {
      Error("esperado IN, BETWEEN ou LIKE depois de NOT");
      return nullptr;
    }
    return left;
  }

  ExprPtr ParseAdditive() {
    ExprPtr left = ParseMultiplicative();
    while (left) {
      std::string op;
      if (AcceptSymbol("+")) {
        op = "+";
      } else if (AcceptSymbol("-")) {
        op = "-";
      } else if (AcceptSymbol("||")) {
        op = "||";
      } else {
        break;
      }
      ExprPtr right = ParseMultiplicative();
      if (!right) return nullptr;
      left = MakeBinary(op, std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr ParseMultiplicative() {
    ExprPtr left = ParseUnary();
    while (left) {
      std::string op;
      if (AcceptSymbol("*")) {
        op = "*";
      } else if (AcceptSymbol("/")) {
        op = "/";
      } else if (AcceptSymbol("%")) {
        op = "%";
      } else {
        break;
      }
      ExprPtr right = ParseUnary();
      if (!right) return nullptr;
      left = MakeBinary(op, std::move(left), std::move(right));
    }
    return left;
  }

  ExprPtr ParseUnary() {
    if (AcceptSymbol("-")) {
      ExprPtr inner = ParseUnary();
      if (!inner) return nullptr;
      if (inner->kind == Expr::Kind::kLiteral && inner->literal.type == Type::kInt) {
        inner->literal.i = -inner->literal.i;
        return inner;
      }
      if (inner->kind == Expr::Kind::kLiteral && inner->literal.type == Type::kReal) {
        inner->literal.d = -inner->literal.d;
        return inner;
      }
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kUnary;
      e->op = "-";
      e->args.push_back(std::move(inner));
      return e;
    }
    return ParsePrimary();
  }

  ExprPtr ParsePrimary() {
    const Token& t = Peek();
    if (AcceptSymbol("(")) {
      ExprPtr e = ParseExpr();
      if (!e || !ExpectSymbol(")")) return nullptr;
      return e;
    }
    if (t.kind == Token::Kind::kNumber || t.kind == Token::Kind::kString || IsKeyword(t, "true") ||
        IsKeyword(t, "false") || IsKeyword(t, "null")) {
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kLiteral;
      if (!ParseLiteral(&e->literal)) return nullptr;
      return e;
    }
    if (t.kind == Token::Kind::kIdent || t.kind == Token::Kind::kQuotedIdent) {
      std::string name;
      ParseName(&name);
      if (AcceptSymbol("(")) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::kFunction;
        e->name = name;
        if (AcceptSymbol("*")) {
          auto star = std::make_unique<Expr>();
          star->kind = Expr::Kind::kStar;
          e->args.push_back(std::move(star));
        } else if (!(Peek().kind == Token::Kind::kSymbol && Peek().text == ")")) {
          e->distinct = AcceptKeyword("distinct");
          do {
            ExprPtr a = ParseExpr();
            if (!a) return nullptr;
            e->args.push_back(std::move(a));
          } while (AcceptSymbol(","));
        }
        if (!ExpectSymbol(")")) return nullptr;
        return e;
      }
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::kColumn;
      e->name = name;
      if (AcceptSymbol(".")) {
        if (!ParseName(&e->name)) return nullptr;
      }
      return e;
    }
    Error("expressão inválida perto de '" + t.text + "'");
    return nullptr;
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
  Status status_;
};

}

Status Parse(std::string_view text, Statement* out) {
  std::vector<Token> tokens;
  Status s = Tokenize(text, &tokens);
  if (!s.ok()) return s;
  if (tokens.size() == 1) return Status::InvalidArgument("consulta vazia");
  Parser parser(std::move(tokens));
  return parser.ParseStatement(out);
}

}
