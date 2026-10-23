package resp

import (
	"bufio"
	"bytes"
	"context"
	"net"
	"strings"
	"testing"
	"time"
)

func TestReadValue(t *testing.T) {
	in := "*5\r\n+OK\r\n-ERR deu ruim\r\n:42\r\n$4\r\nolá\r\n*2\r\n$-1\r\n*0\r\n"
	v, err := ReadValue(bufio.NewReader(strings.NewReader(in)))
	if err != nil {
		t.Fatal(err)
	}
	if v.Kind != Array || len(v.Array) != 5 {
		t.Fatalf("valor inesperado: %+v", v)
	}
	if v.Array[0].Str != "OK" || v.Array[1].Err() == nil || v.Array[2].Int != 42 || v.Array[3].Str != "olá" {
		t.Fatalf("itens inesperados: %+v", v.Array)
	}
	if !v.Array[4].Array[0].IsNull() || len(v.Array[4].Array[1].Array) != 0 {
		t.Fatalf("aninhados inesperados: %+v", v.Array[4])
	}
	j := v.ToJSON().([]any)
	if j[1].(map[string]any)["error"] != "ERR deu ruim" || j[2].(int64) != 42 {
		t.Fatalf("json inesperado: %v", j)
	}
	if (&ServerError{Message: "ERR tabela não existe"}).Text() != "tabela não existe" {
		t.Fatal("prefixo não removido")
	}
	for _, bad := range []string{"?x\r\n", "$3\r\nab\r\n", ":12\n", "$9999999999\r\n"} {
		if _, err := ReadValue(bufio.NewReader(strings.NewReader(bad))); err == nil {
			t.Fatalf("entrada inválida aceita: %q", bad)
		}
	}
}

func TestWriteCommandRoundTrip(t *testing.T) {
	var buf bytes.Buffer
	w := bufio.NewWriter(&buf)
	if err := WriteCommand(w, []string{"SET", "chave com espaço", "a\r\nb"}); err != nil {
		t.Fatal(err)
	}
	args, err := ReadCommand(bufio.NewReader(&buf))
	if err != nil || len(args) != 3 || args[2] != "a\r\nb" {
		t.Fatalf("ida e volta falhou: %v %v", args, err)
	}
}

func TestClientPoolAndAuth(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	accepted := make(chan struct{}, 16)
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			accepted <- struct{}{}
			go func(c net.Conn) {
				defer c.Close()
				r, w := bufio.NewReader(c), bufio.NewWriter(c)
				authed := false
				for {
					args, err := ReadCommand(r)
					if err != nil {
						return
					}
					var v Value
					switch strings.ToUpper(args[0]) {
					case "AUTH":
						authed = args[1] == "s3nha"
						v = Value{Kind: SimpleString, Str: "OK"}
						if !authed {
							v = Value{Kind: Error, Str: "WRONGPASS senha inválida"}
						}
					case "PING":
						v = Value{Kind: SimpleString, Str: "PONG"}
						if !authed {
							v = Value{Kind: Error, Str: "NOAUTH"}
						}
					case "QUIT":
						return
					case "JSON":
						v = Value{Kind: Bulk, Str: `{"n":12345678901234}`}
					}
					WriteValue(w, v)
					w.Flush()
				}
			}(c)
		}
	}()
	c := NewClient(Options{Addr: ln.Addr().String(), Password: "s3nha", PoolSize: 2})
	defer c.Close()
	ctx := context.Background()
	for i := 0; i < 5; i++ {
		if err := c.Ping(ctx); err != nil {
			t.Fatal(err)
		}
	}
	if len(accepted) != 1 {
		t.Fatalf("pool deveria reutilizar a conexão, abriu %d", len(accepted))
	}
	var out struct {
		N any `json:"n"`
	}
	if err := c.JSON(ctx, &out, "JSON"); err != nil || out.N.(interface{ String() string }).String() != "12345678901234" {
		t.Fatalf("JSON com número grande falhou: %v %v", out, err)
	}
	if _, err := c.Do(ctx, "QUIT"); err == nil {
		t.Fatal("esperado erro com a conexão fechada pelo servidor")
	}
	if err := c.Ping(ctx); err != nil {
		t.Fatalf("pool não se recuperou: %v", err)
	}
	bad := NewClient(Options{Addr: ln.Addr().String(), Password: "errada", DialTimeout: time.Second})
	defer bad.Close()
	if err := bad.Ping(ctx); err == nil {
		t.Fatal("senha errada deveria falhar")
	}
}
