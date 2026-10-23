package store

import (
	"encoding/json"
	"testing"
)

func TestLiteral(t *testing.T) {
	cases := []struct {
		typ  string
		in   any
		want string
		bad  bool
	}{
		{"TEXT", "O'Neil", "'O''Neil'", false},
		{"TEXT", json.Number("12"), "'12'", false},
		{"INT", json.Number("42"), "42", false},
		{"INT", json.Number("-7"), "-7", false},
		{"INT", json.Number("4.5"), "", true},
		{"INT", " 15 ", "15", false},
		{"INT", "abc", "", true},
		{"REAL", json.Number("3"), "3.0", false},
		{"REAL", "19,90", "19.9", false},
		{"REAL", json.Number("0.000001"), "0.000001", false},
		{"BOOL", true, "TRUE", false},
		{"BOOL", "não", "FALSE", false},
		{"BOOL", "talvez", "", true},
		{"TEXT", nil, "NULL", false},
		{"BLOB", "x", "", true},
	}
	for _, c := range cases {
		got, err := Literal(c.typ, c.in)
		if c.bad {
			if err == nil {
				t.Fatalf("%s %v deveria falhar, veio %s", c.typ, c.in, got)
			}
			continue
		}
		if err != nil || got != c.want {
			t.Fatalf("%s %v: veio %q (%v), esperado %q", c.typ, c.in, got, err, c.want)
		}
	}
}

func TestStatementKind(t *testing.T) {
	for q, ro := range map[string]bool{
		"select * from t":      true,
		"  (SELECT 1)":         true,
		"EXPLAIN SELECT 1":     true,
		"describe t":           true,
		"SHOW TABLES":          true,
		"INSERT INTO t VALUES": false,
		"drop table t":         false,
		"":                     false,
	} {
		if IsReadOnly(q) != ro {
			t.Fatalf("%q: esperado %v", q, ro)
		}
	}
	if !ValidIdent("clientes_2026") || ValidIdent("1x") || ValidIdent("a-b") || ValidIdent("") {
		t.Fatal("validação de identificador errada")
	}
}
