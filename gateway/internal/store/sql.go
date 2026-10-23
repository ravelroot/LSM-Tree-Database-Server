package store

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
)

var identRe = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]{0,63}$`)

func ValidIdent(name string) bool {
	return identRe.MatchString(name)
}

func Quote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", "''") + "'"
}

func IntLiteral(n int64) string {
	return strconv.FormatInt(n, 10)
}

func RealLiteral(f float64) (string, error) {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return "", errors.New("número inválido")
	}
	s := strconv.FormatFloat(f, 'f', -1, 64)
	if !strings.ContainsAny(s, ".") {
		s += ".0"
	}
	return s, nil
}

func BoolLiteral(b bool) string {
	if b {
		return "TRUE"
	}
	return "FALSE"
}

func Literal(colType string, v any) (string, error) {
	if v == nil {
		return "NULL", nil
	}
	switch strings.ToUpper(colType) {
	case "INT":
		switch x := v.(type) {
		case json.Number:
			n, err := strconv.ParseInt(x.String(), 10, 64)
			if err != nil {
				f, ferr := strconv.ParseFloat(x.String(), 64)
				if ferr != nil || f != math.Trunc(f) || math.Abs(f) > 9e15 {
					return "", errors.New("esperado um número inteiro")
				}
				n = int64(f)
			}
			return IntLiteral(n), nil
		case float64:
			if x != math.Trunc(x) || math.Abs(x) > 9e15 {
				return "", errors.New("esperado um número inteiro")
			}
			return IntLiteral(int64(x)), nil
		case int64:
			return IntLiteral(x), nil
		case int:
			return IntLiteral(int64(x)), nil
		case string:
			s := strings.TrimSpace(x)
			if s == "" {
				return "NULL", nil
			}
			n, err := strconv.ParseInt(s, 10, 64)
			if err != nil {
				return "", errors.New("esperado um número inteiro")
			}
			return IntLiteral(n), nil
		case bool:
			if x {
				return "1", nil
			}
			return "0", nil
		}
	case "REAL":
		switch x := v.(type) {
		case json.Number:
			f, err := strconv.ParseFloat(x.String(), 64)
			if err != nil {
				return "", errors.New("esperado um número")
			}
			return RealLiteral(f)
		case float64:
			return RealLiteral(x)
		case int64:
			return RealLiteral(float64(x))
		case int:
			return RealLiteral(float64(x))
		case string:
			s := strings.TrimSpace(strings.ReplaceAll(x, ",", "."))
			if s == "" {
				return "NULL", nil
			}
			f, err := strconv.ParseFloat(s, 64)
			if err != nil {
				return "", errors.New("esperado um número")
			}
			return RealLiteral(f)
		}
	case "BOOL":
		switch x := v.(type) {
		case bool:
			return BoolLiteral(x), nil
		case string:
			switch strings.ToLower(strings.TrimSpace(x)) {
			case "true", "1", "sim", "verdadeiro":
				return "TRUE", nil
			case "false", "0", "não", "nao", "falso":
				return "FALSE", nil
			case "":
				return "NULL", nil
			}
			return "", errors.New("esperado verdadeiro ou falso")
		case json.Number:
			if x.String() == "0" {
				return "FALSE", nil
			}
			return "TRUE", nil
		}
	case "TEXT":
		switch x := v.(type) {
		case string:
			return Quote(x), nil
		case json.Number:
			return Quote(x.String()), nil
		case bool:
			return Quote(strconv.FormatBool(x)), nil
		case float64:
			return Quote(strconv.FormatFloat(x, 'f', -1, 64)), nil
		case int64:
			return Quote(strconv.FormatInt(x, 10)), nil
		}
	default:
		return "", fmt.Errorf("tipo desconhecido %q", colType)
	}
	return "", fmt.Errorf("valor incompatível com %s", strings.ToUpper(colType))
}

func StatementKind(query string) string {
	q := strings.TrimLeft(query, " \t\r\n(")
	end := strings.IndexFunc(q, func(r rune) bool {
		return !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z')
	})
	if end < 0 {
		end = len(q)
	}
	return strings.ToUpper(q[:end])
}

func IsReadOnly(query string) bool {
	switch StatementKind(query) {
	case "SELECT", "EXPLAIN", "SHOW", "DESCRIBE", "DESC":
		return true
	}
	return false
}

func Cell(v any) string {
	switch x := v.(type) {
	case nil:
		return ""
	case string:
		return x
	case json.Number:
		return x.String()
	case bool:
		return strconv.FormatBool(x)
	case float64:
		return strconv.FormatFloat(x, 'f', -1, 64)
	}
	return fmt.Sprint(v)
}

func CellInt(v any) int64 {
	switch x := v.(type) {
	case json.Number:
		if n, err := x.Int64(); err == nil {
			return n
		}
		if f, err := x.Float64(); err == nil {
			return int64(f)
		}
	case float64:
		return int64(x)
	case int64:
		return x
	case bool:
		if x {
			return 1
		}
	case string:
		n, _ := strconv.ParseInt(x, 10, 64)
		return n
	}
	return 0
}
