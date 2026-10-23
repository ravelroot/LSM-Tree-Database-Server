package resp

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
)

type Kind int

const (
	SimpleString Kind = iota
	Error
	Integer
	Bulk
	Null
	Array
)

const maxBulkLen = 512 << 20

type Value struct {
	Kind  Kind
	Str   string
	Int   int64
	Array []Value
}

type ServerError struct {
	Message string
}

func (e *ServerError) Error() string {
	return e.Message
}

func (e *ServerError) Text() string {
	msg := e.Message
	for _, prefix := range []string{"ERR ", "WRONGPASS ", "NOAUTH ", "WRONGTYPE ", "EXECABORT "} {
		if strings.HasPrefix(msg, prefix) {
			return strings.TrimSpace(strings.TrimPrefix(msg, prefix))
		}
	}
	return msg
}

var ErrProtocol = errors.New("resposta RESP inválida")

func (v Value) Err() error {
	if v.Kind == Error {
		return &ServerError{Message: v.Str}
	}
	return nil
}

func (v Value) IsNull() bool {
	return v.Kind == Null
}

func (v Value) Text() string {
	switch v.Kind {
	case SimpleString, Bulk, Error:
		return v.Str
	case Integer:
		return strconv.FormatInt(v.Int, 10)
	}
	return ""
}

func (v Value) ToJSON() any {
	switch v.Kind {
	case SimpleString, Bulk:
		return v.Str
	case Error:
		return map[string]any{"error": v.Str}
	case Integer:
		return v.Int
	case Array:
		out := make([]any, len(v.Array))
		for i, item := range v.Array {
			out[i] = item.ToJSON()
		}
		return out
	}
	return nil
}

func WriteCommand(w *bufio.Writer, args []string) error {
	if _, err := fmt.Fprintf(w, "*%d\r\n", len(args)); err != nil {
		return err
	}
	for _, arg := range args {
		if _, err := fmt.Fprintf(w, "$%d\r\n", len(arg)); err != nil {
			return err
		}
		if _, err := w.WriteString(arg); err != nil {
			return err
		}
		if _, err := w.WriteString("\r\n"); err != nil {
			return err
		}
	}
	return w.Flush()
}

func WriteValue(w *bufio.Writer, v Value) error {
	var err error
	switch v.Kind {
	case SimpleString:
		_, err = fmt.Fprintf(w, "+%s\r\n", v.Str)
	case Error:
		_, err = fmt.Fprintf(w, "-%s\r\n", v.Str)
	case Integer:
		_, err = fmt.Fprintf(w, ":%d\r\n", v.Int)
	case Bulk:
		_, err = fmt.Fprintf(w, "$%d\r\n%s\r\n", len(v.Str), v.Str)
	case Null:
		_, err = w.WriteString("$-1\r\n")
	case Array:
		if _, err = fmt.Fprintf(w, "*%d\r\n", len(v.Array)); err != nil {
			return err
		}
		for _, item := range v.Array {
			if err = WriteValue(w, item); err != nil {
				return err
			}
		}
	}
	return err
}

func readLine(r *bufio.Reader) (string, error) {
	line, err := r.ReadString('\n')
	if err != nil {
		return "", err
	}
	if len(line) < 2 || line[len(line)-2] != '\r' {
		return "", ErrProtocol
	}
	return line[:len(line)-2], nil
}

func ReadValue(r *bufio.Reader) (Value, error) {
	line, err := readLine(r)
	if err != nil {
		return Value{}, err
	}
	if line == "" {
		return Value{}, ErrProtocol
	}
	body := line[1:]
	switch line[0] {
	case '+':
		return Value{Kind: SimpleString, Str: body}, nil
	case '-':
		return Value{Kind: Error, Str: body}, nil
	case ':':
		n, err := strconv.ParseInt(body, 10, 64)
		if err != nil {
			return Value{}, ErrProtocol
		}
		return Value{Kind: Integer, Int: n}, nil
	case '$':
		n, err := strconv.Atoi(body)
		if err != nil || n > maxBulkLen {
			return Value{}, ErrProtocol
		}
		if n < 0 {
			return Value{Kind: Null}, nil
		}
		buf := make([]byte, n+2)
		if _, err := io.ReadFull(r, buf); err != nil {
			return Value{}, err
		}
		if buf[n] != '\r' || buf[n+1] != '\n' {
			return Value{}, ErrProtocol
		}
		return Value{Kind: Bulk, Str: string(buf[:n])}, nil
	case '*':
		n, err := strconv.Atoi(body)
		if err != nil || n > 1<<20 {
			return Value{}, ErrProtocol
		}
		if n < 0 {
			return Value{Kind: Null}, nil
		}
		items := make([]Value, n)
		for i := range items {
			item, err := ReadValue(r)
			if err != nil {
				return Value{}, err
			}
			items[i] = item
		}
		return Value{Kind: Array, Array: items}, nil
	case '_':
		return Value{Kind: Null}, nil
	}
	return Value{}, ErrProtocol
}

func ReadCommand(r *bufio.Reader) ([]string, error) {
	v, err := ReadValue(r)
	if err != nil {
		return nil, err
	}
	if v.Kind == SimpleString {
		return strings.Fields(v.Str), nil
	}
	if v.Kind != Array {
		return nil, ErrProtocol
	}
	args := make([]string, len(v.Array))
	for i, item := range v.Array {
		if item.Kind != Bulk && item.Kind != SimpleString {
			return nil, ErrProtocol
		}
		args[i] = item.Str
	}
	return args, nil
}
