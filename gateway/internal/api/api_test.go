package api

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"arca/gateway/internal/config"
	"arca/gateway/internal/resp"
	"arca/gateway/internal/seed"
	"arca/gateway/internal/store"
)

type testEnv struct {
	t    *testing.T
	fake *fakeArca
	srv  *Server
	ts   *httptest.Server
	web  string
}

func setup(t *testing.T) *testEnv {
	t.Helper()
	fake := newFakeArca(t, "segredo-de-teste")
	client := resp.NewClient(resp.Options{Addr: fake.addr(), Password: "segredo-de-teste", PoolSize: 4})
	st := store.New(client)
	web := t.TempDir()
	cfg := config.Config{SessionTTL: time.Hour, WebDir: web}
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	srv := New(cfg, client, st, logger)
	ctx := context.Background()
	if err := st.Bootstrap(ctx); err != nil {
		t.Fatal(err)
	}
	created, err := (&seed.Seeder{Store: st, Now: time.Now}).Run(ctx)
	if err != nil || !created {
		t.Fatalf("seed: created=%v err=%v", created, err)
	}
	srv.SetReady(true)
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	t.Cleanup(client.Close)
	return &testEnv{t: t, fake: fake, srv: srv, ts: ts, web: web}
}

type result struct {
	status int
	body   []byte
	header http.Header
}

func (r result) json(t *testing.T, out any) {
	t.Helper()
	if err := json.Unmarshal(r.body, out); err != nil {
		t.Fatalf("json inválido (%d): %s", r.status, r.body)
	}
}

func (e *testEnv) call(method, path, token string, body any) result {
	e.t.Helper()
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, _ := http.NewRequest(method, e.ts.URL+path, rd)
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	res, err := e.ts.Client().Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer res.Body.Close()
	data, _ := io.ReadAll(res.Body)
	return result{status: res.StatusCode, body: data, header: res.Header}
}

func (e *testEnv) login(email string) string {
	e.t.Helper()
	r := e.call("POST", "/api/auth/login", "", map[string]string{"email": email, "password": seed.DemoPassword})
	if r.status != 200 {
		e.t.Fatalf("login %s: %d %s", email, r.status, r.body)
	}
	var out struct {
		Token string `json:"token"`
		User  struct {
			Role string `json:"role"`
		} `json:"user"`
	}
	r.json(e.t, &out)
	if out.Token == "" {
		e.t.Fatal("token vazio")
	}
	return out.Token
}

func expect(t *testing.T, r result, status int) {
	t.Helper()
	if r.status != status {
		t.Fatalf("status %d, esperado %d: %s", r.status, status, r.body)
	}
}

func TestLoginMeLogout(t *testing.T) {
	e := setup(t)
	token := e.login("ADMIN@arca.dev ")
	r := e.call("GET", "/api/auth/me", token, nil)
	expect(t, r, 200)
	var me map[string]any
	r.json(t, &me)
	if me["role"] != "admin" || me["email"] != "admin@arca.dev" {
		t.Fatalf("me inesperado: %v", me)
	}
	if _, ok := me["password"]; ok {
		t.Fatal("senha exposta")
	}
	expect(t, e.call("POST", "/api/auth/logout", token, nil), 204)
	expect(t, e.call("GET", "/api/auth/me", token, nil), 401)
	expect(t, e.call("GET", "/api/auth/me", "", nil), 401)
	for k := range e.fake.kv {
		if strings.Contains(k, token) {
			t.Fatal("token salvo em claro")
		}
	}
}

func TestLoginLockout(t *testing.T) {
	e := setup(t)
	for i := 1; i <= 5; i++ {
		r := e.call("POST", "/api/auth/login", "", map[string]string{"email": "leitura@arca.dev", "password": "errada123"})
		want := 401
		if i == 5 {
			want = 423
		}
		expect(t, r, want)
	}
	r := e.call("POST", "/api/auth/login", "", map[string]string{"email": "leitura@arca.dev", "password": seed.DemoPassword})
	expect(t, r, 423)
	admin := e.login("admin@arca.dev")
	expect(t, e.call("PATCH", "/api/users/leitura@arca.dev", admin, map[string]any{"unlock": true}), 200)
	e.login("leitura@arca.dev")
	r = e.call("GET", "/api/audit?limit=50", admin, nil)
	expect(t, r, 200)
	if !bytes.Contains(r.body, []byte("login_falhou")) {
		t.Fatal("falhas de login não auditadas")
	}
	r = e.call("POST", "/api/auth/login", "", map[string]string{"email": "ninguem@arca.dev", "password": "qualquer1"})
	expect(t, r, 401)
}

func TestRoles(t *testing.T) {
	e := setup(t)
	reader := e.login("leitura@arca.dev")
	operator := e.login("operador@arca.dev")
	expect(t, e.call("PUT", "/api/keys/value", reader, map[string]any{"key": "x", "value": "1"}), 403)
	expect(t, e.call("POST", "/api/sql", reader, map[string]string{"query": "DELETE FROM clientes"}), 403)
	expect(t, e.call("POST", "/api/sql", reader, map[string]string{"query": "SELECT COUNT(*) FROM clientes"}), 200)
	expect(t, e.call("POST", "/api/sql", operator, map[string]string{"query": "SELECT * FROM _painel_usuarios"}), 403)
	expect(t, e.call("GET", "/api/users", operator, nil), 403)
	expect(t, e.call("POST", "/api/command", operator, map[string]any{"args": []string{"PING"}}), 403)
	expect(t, e.call("GET", "/api/config", operator, nil), 403)
	expect(t, e.call("PUT", "/api/keys/value", operator, map[string]any{"key": "x", "value": "1"}), 204)
}

func TestKeys(t *testing.T) {
	e := setup(t)
	op := e.login("operador@arca.dev")
	expect(t, e.call("PUT", "/api/keys/value", op, map[string]any{"key": "teste:a", "value": "olá", "ttl_s": 120}), 204)
	r := e.call("GET", "/api/keys/value?key="+url.QueryEscape("teste:a"), op, nil)
	expect(t, r, 200)
	var kv map[string]any
	r.json(t, &kv)
	if kv["value"] != "olá" || kv["ttl_ms"].(float64) <= 0 || kv["size"].(float64) != float64(len("olá")) {
		t.Fatalf("valor inesperado: %v", kv)
	}
	expect(t, e.call("POST", "/api/keys/expire", op, map[string]any{"key": "teste:a", "ttl_s": 0}), 204)
	r = e.call("GET", "/api/keys/value?key=teste:a", op, nil)
	r.json(t, &kv)
	if kv["ttl_ms"].(float64) != -1 {
		t.Fatalf("TTL não removido: %v", kv)
	}
	r = e.call("GET", "/api/keys?match=teste:*&count=10", op, nil)
	expect(t, r, 200)
	var page struct {
		Cursor string `json:"cursor"`
		Keys   []struct {
			Key string `json:"key"`
		} `json:"keys"`
	}
	r.json(t, &page)
	if len(page.Keys) != 1 || page.Keys[0].Key != "teste:a" || page.Cursor != "0" {
		t.Fatalf("listagem inesperada: %s", r.body)
	}
	r = e.call("GET", "/api/keys?match=*&count=1000", op, nil)
	if bytes.Contains(r.body, []byte("_painel:")) {
		t.Fatal("sessões apareceram na listagem")
	}
	expect(t, e.call("GET", "/api/keys/value?key=_painel:sessao:abc", op, nil), 403)
	expect(t, e.call("DELETE", "/api/keys/value?key=teste:a", op, nil), 204)
	expect(t, e.call("DELETE", "/api/keys/value?key=teste:a", op, nil), 404)
	expect(t, e.call("GET", "/api/keys/value?key=teste:a", op, nil), 404)
	expect(t, e.call("POST", "/api/keys/expire", op, map[string]any{"key": "teste:a", "ttl_s": 10}), 404)
	expect(t, e.call("PUT", "/api/keys/value", op, map[string]any{"key": "y"}), 400)
}

func TestTablesAndRows(t *testing.T) {
	e := setup(t)
	op := e.login("operador@arca.dev")
	r := e.call("GET", "/api/tables", op, nil)
	expect(t, r, 200)
	var tables []store.Table
	r.json(t, &tables)
	names := map[string]int64{}
	for _, tb := range tables {
		names[tb.Name] = tb.Rows
	}
	if names["clientes"] != 150 || names["pedidos"] != 420 || len(names) != 3 {
		t.Fatalf("tabelas inesperadas: %v", names)
	}
	r = e.call("GET", "/api/tables/clientes/rows?limit=10&offset=5&order=id&dir=desc", op, nil)
	expect(t, r, 200)
	var page struct {
		Columns    []string `json:"columns"`
		Rows       [][]any  `json:"rows"`
		Total      int64    `json:"total"`
		PrimaryKey string   `json:"primary_key"`
	}
	r.json(t, &page)
	if len(page.Rows) != 10 || page.Total != 150 || page.PrimaryKey != "id" || page.Rows[0][0].(float64) != 145 {
		t.Fatalf("página inesperada: %s", r.body)
	}
	r = e.call("GET", "/api/tables/clientes/rows?filter="+url.QueryEscape("id <= 3"), op, nil)
	r.json(t, &page)
	if page.Total != 3 || len(page.Rows) != 3 {
		t.Fatalf("filtro inesperado: %s", r.body)
	}
	expect(t, e.call("GET", "/api/tables/clientes/rows?filter="+url.QueryEscape("1=1; DROP TABLE x"), op, nil), 400)
	expect(t, e.call("GET", "/api/tables/_painel_usuarios/rows", op, nil), 404)
	expect(t, e.call("GET", "/api/tables/inexistente/rows", op, nil), 404)
	expect(t, e.call("POST", "/api/tables/clientes/rows", op, map[string]any{"values": map[string]any{"nome": "Cliente O'Teste", "cidade": "Recife", "ativo": true}}), 201)
	r = e.call("POST", "/api/sql", op, map[string]string{"query": "SELECT nome FROM clientes WHERE id = 151"})
	if !bytes.Contains(r.body, []byte("O'Teste")) {
		t.Fatalf("inserção com aspas falhou: %s", r.body)
	}
	expect(t, e.call("POST", "/api/tables/clientes/rows", op, map[string]any{"values": map[string]any{"cidade": "Recife"}}), 400)
	expect(t, e.call("POST", "/api/tables/clientes/rows", op, map[string]any{"values": map[string]any{"nome": "x", "naoexiste": 1}}), 400)
	expect(t, e.call("PUT", "/api/tables/clientes/rows", op, map[string]any{"key": 151, "values": map[string]any{"cidade": "Natal"}}), 204)
	expect(t, e.call("PUT", "/api/tables/clientes/rows", op, map[string]any{"key": 9999, "values": map[string]any{"cidade": "Natal"}}), 404)
	expect(t, e.call("PUT", "/api/tables/clientes/rows", op, map[string]any{"key": "abc", "values": map[string]any{"cidade": "Natal"}}), 400)
	expect(t, e.call("DELETE", "/api/tables/clientes/rows?key=151", op, nil), 204)
	expect(t, e.call("DELETE", "/api/tables/clientes/rows?key=151", op, nil), 404)
}

func TestSQLAndCommand(t *testing.T) {
	e := setup(t)
	admin := e.login("admin@arca.dev")
	r := e.call("POST", "/api/sql", admin, map[string]string{"query": "  SELECT COUNT(*) FROM pedidos; "})
	expect(t, r, 200)
	var res map[string]any
	r.json(t, &res)
	if res["kind"] != "rows" {
		t.Fatalf("resultado inesperado: %s", r.body)
	}
	r = e.call("POST", "/api/sql", admin, map[string]string{"query": "SELEC x"})
	expect(t, r, 400)
	var apiErr apiError
	r.json(t, &apiErr)
	if apiErr.Code != "arca_error" || strings.HasPrefix(apiErr.Message, "ERR") {
		t.Fatalf("erro inesperado: %s", r.body)
	}
	expect(t, e.call("POST", "/api/sql", admin, map[string]string{"query": "UPDATE produtos SET estoque = 5 WHERE id = 1"}), 200)
	r = e.call("POST", "/api/command", admin, map[string]any{"args": []string{"PING"}})
	expect(t, r, 200)
	var cmd map[string]any
	r.json(t, &cmd)
	if cmd["reply"] != "PONG" {
		t.Fatalf("resposta inesperada: %s", r.body)
	}
	r = e.call("POST", "/api/command", admin, map[string]any{"args": []string{"NAOEXISTE"}})
	r.json(t, &cmd)
	if m, ok := cmd["reply"].(map[string]any); !ok || m["error"] == nil {
		t.Fatalf("erro não convertido: %s", r.body)
	}
	expect(t, e.call("POST", "/api/command", admin, map[string]any{"args": []string{}}), 400)
	r = e.call("GET", "/api/audit", admin, nil)
	if !bytes.Contains(r.body, []byte("UPDATE produtos")) || !bytes.Contains(r.body, []byte(`"comando"`)) {
		t.Fatalf("auditoria incompleta: %s", r.body)
	}
}

func TestMonitoringAndAdmin(t *testing.T) {
	e := setup(t)
	admin := e.login("admin@arca.dev")
	r := e.call("GET", "/api/overview", admin, nil)
	expect(t, r, 200)
	var ov map[string]any
	r.json(t, &ov)
	if ov["tables"].(float64) != 3 || ov["engine"] == nil {
		t.Fatalf("overview inesperado: %s", r.body)
	}
	r = e.call("GET", "/api/storage", admin, nil)
	expect(t, r, 200)
	var storage map[string]json.RawMessage
	r.json(t, &storage)
	if storage["levels"] == nil || storage["recent_compactions"] == nil {
		t.Fatalf("storage inesperado: %s", r.body)
	}
	for _, p := range []string{"flush", "compact", "checkpoint"} {
		expect(t, e.call("POST", "/api/storage/"+p, admin, nil), 200)
	}
	r = e.call("GET", "/api/clients", admin, nil)
	var clients []clientInfo
	r.json(t, &clients)
	if len(clients) != 2 || clients[0].Name != "painel" || clients[1].IdleS != 2 {
		t.Fatalf("clientes inesperados: %s", r.body)
	}
	r = e.call("GET", "/api/slowlog", admin, nil)
	var slow []slowEntry
	r.json(t, &slow)
	if len(slow) != 1 || slow[0].Micros != 15000 || slow[0].At != 1761170000000 || len(slow[0].Args) != 2 {
		t.Fatalf("slowlog inesperado: %s", r.body)
	}
	r = e.call("GET", "/api/config", admin, nil)
	if bytes.Contains(r.body, []byte("requirepass")) {
		t.Fatal("senha do servidor exposta")
	}
	expect(t, e.call("PUT", "/api/config", admin, map[string]string{"name": "appendfsync", "value": "always"}), 204)
	expect(t, e.call("PUT", "/api/config", admin, map[string]string{"name": "naoexiste", "value": "1"}), 400)
	expect(t, e.call("PUT", "/api/config", admin, map[string]string{"name": "requirepass", "value": "x"}), 403)
}

func TestUsers(t *testing.T) {
	e := setup(t)
	admin := e.login("admin@arca.dev")
	r := e.call("POST", "/api/users", admin, map[string]string{"email": "invalido", "name": "", "role": "chefe", "password": "curta"})
	expect(t, r, 400)
	var apiErr apiError
	r.json(t, &apiErr)
	if len(apiErr.Fields) != 4 {
		t.Fatalf("campos esperados: %s", r.body)
	}
	expect(t, e.call("POST", "/api/users", admin, map[string]string{"email": "nova@arca.dev", "name": "Nova", "role": "reader", "password": "senhaNova1"}), 201)
	expect(t, e.call("POST", "/api/users", admin, map[string]string{"email": "nova@arca.dev", "name": "Nova", "role": "reader", "password": "senhaNova1"}), 409)
	r = e.call("POST", "/api/auth/login", "", map[string]string{"email": "nova@arca.dev", "password": "senhaNova1"})
	expect(t, r, 200)
	var login struct {
		Token string `json:"token"`
	}
	r.json(t, &login)
	expect(t, e.call("PATCH", "/api/users/nova@arca.dev", admin, map[string]any{"role": "operator", "name": "Nova Operadora"}), 200)
	r = e.call("GET", "/api/auth/me", login.Token, nil)
	if !bytes.Contains(r.body, []byte(`"operator"`)) {
		t.Fatalf("papel não atualizado na sessão: %s", r.body)
	}
	expect(t, e.call("PATCH", "/api/users/admin@arca.dev", admin, map[string]any{"role": "reader"}), 409)
	expect(t, e.call("DELETE", "/api/users/admin@arca.dev", admin, nil), 409)
	expect(t, e.call("PUT", "/api/auth/me/password", login.Token, map[string]string{"current": "errada", "new": "outraSenha2"}), 400)
	expect(t, e.call("PUT", "/api/auth/me/password", login.Token, map[string]string{"current": "senhaNova1", "new": "outraSenha2"}), 204)
	expect(t, e.call("DELETE", "/api/users/nova@arca.dev", admin, nil), 204)
	expect(t, e.call("GET", "/api/auth/me", login.Token, nil), 401)
	expect(t, e.call("DELETE", "/api/users/nova@arca.dev", admin, nil), 404)
	r = e.call("GET", "/api/users", admin, nil)
	if bytes.Contains(r.body, []byte("pbkdf2")) {
		t.Fatal("hash de senha exposto")
	}
}

func TestPublicRoutesAndHeaders(t *testing.T) {
	e := setup(t)
	r := e.call("GET", "/api/health", "", nil)
	expect(t, r, 200)
	if !bytes.Contains(r.body, []byte(`"arca":"ok"`)) {
		t.Fatalf("health inesperado: %s", r.body)
	}
	if r.header.Get("X-Frame-Options") != "DENY" || r.header.Get("Cache-Control") != "no-store" || !strings.Contains(r.header.Get("Content-Security-Policy"), "default-src 'self'") {
		t.Fatalf("cabeçalhos ausentes: %v", r.header)
	}
	r = e.call("GET", "/api/openapi.json", "", nil)
	expect(t, r, 200)
	var doc map[string]any
	r.json(t, &doc)
	paths := doc["paths"].(map[string]any)
	if len(paths) < 20 || paths["/api/tables/{name}/rows"] == nil {
		t.Fatalf("openapi incompleta: %d rotas", len(paths))
	}
	expect(t, e.call("GET", "/api/naoexiste", "", nil), 404)
	r = e.call("POST", "/api/auth/login", "", "isto não é um objeto")
	expect(t, r, 400)
}

func TestStaticFallback(t *testing.T) {
	e := setup(t)
	expect(t, e.call("GET", "/", "", nil), 404)
	os.MkdirAll(filepath.Join(e.web, "assets"), 0o755)
	os.WriteFile(filepath.Join(e.web, "index.html"), []byte("<!doctype html><title>Arca</title>"), 0o644)
	os.WriteFile(filepath.Join(e.web, "assets", "app.js"), []byte("console.log(1)"), 0o644)
	r := e.call("GET", "/tabelas", "", nil)
	expect(t, r, 200)
	if !bytes.Contains(r.body, []byte("<title>Arca</title>")) {
		t.Fatal("fallback SPA não serviu o index")
	}
	r = e.call("GET", "/assets/app.js", "", nil)
	expect(t, r, 200)
	if !strings.Contains(r.header.Get("Cache-Control"), "immutable") {
		t.Fatal("assets sem cache longo")
	}
	expect(t, e.call("GET", "/assets/faltando.js", "", nil), 404)
	expect(t, e.call("GET", "/../../etc/passwd", "", nil), 200)
}

func TestEvents(t *testing.T) {
	e := setup(t)
	token := e.login("leitura@arca.dev")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", e.ts.URL+"/api/events?token="+url.QueryEscape(token), nil)
	res, err := e.ts.Client().Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	if res.StatusCode != 200 || !strings.HasPrefix(res.Header.Get("Content-Type"), "text/event-stream") {
		t.Fatalf("SSE inesperado: %d %s", res.StatusCode, res.Header.Get("Content-Type"))
	}
	sc := bufio.NewScanner(res.Body)
	var event, data string
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "event: ") {
			event = strings.TrimPrefix(line, "event: ")
		}
		if strings.HasPrefix(line, "data: ") && event == "metrics" {
			data = strings.TrimPrefix(line, "data: ")
			break
		}
	}
	var m Metrics
	if err := json.Unmarshal([]byte(data), &m); err != nil {
		t.Fatalf("métrica inválida %q: %v", data, err)
	}
	if len(m.LevelFiles) != 7 || m.SSTFiles != 3 || m.Clients != 2 {
		t.Fatalf("métrica inesperada: %+v", m)
	}
	cancel()
	deadline := time.Now().Add(2 * time.Second)
	for e.srv.Hub().Subscribers() != 0 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if n := e.srv.Hub().Subscribers(); n != 0 {
		t.Fatalf("assinante não removido: %d", n)
	}
	r := e.call("GET", "/api/events?token=invalido", "", nil)
	expect(t, r, 401)
}

func TestComputeMetrics(t *testing.T) {
	prev := &statsSample{}
	prev.Commands.Reads, prev.Commands.Writes = 100, 50
	prev.Engine.BlockCacheHits, prev.Engine.BlockCacheMisses = 10, 10
	cur := &statsSample{}
	cur.Commands.Reads, cur.Commands.Writes = 300, 150
	cur.Engine.BlockCacheHits, cur.Engine.BlockCacheMisses = 40, 10
	m := computeMetrics(prev, cur, 2*time.Second)
	if m.ReadsPerSec != 100 || m.WritesPerSec != 50 || m.CacheHitRatio != 1 {
		t.Fatalf("métricas inesperadas: %+v", m)
	}
	first := computeMetrics(nil, cur, 0)
	if first.ReadsPerSec != 0 || first.CacheHitRatio != 0.8 {
		t.Fatalf("primeira amostra inesperada: %+v", first)
	}
}

func TestArcaUnavailable(t *testing.T) {
	client := resp.NewClient(resp.Options{Addr: "127.0.0.1:1", DialTimeout: 200 * time.Millisecond})
	defer client.Close()
	st := store.New(client)
	srv := New(config.Config{SessionTTL: time.Hour, WebDir: t.TempDir()}, client, st, slog.New(slog.NewTextHandler(io.Discard, nil)))
	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()
	res, err := http.Post(ts.URL+"/api/auth/login", "application/json", strings.NewReader(`{"email":"a@arca.dev","password":"x"}`))
	if err != nil {
		t.Fatal(err)
	}
	res.Body.Close()
	if res.StatusCode != 503 {
		t.Fatalf("esperado 503 antes de pronto, veio %d", res.StatusCode)
	}
	srv.SetReady(true)
	req, _ := http.NewRequest("GET", ts.URL+"/api/overview", nil)
	req.Header.Set("Authorization", "Bearer qualquer")
	res, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(res.Body)
	res.Body.Close()
	if res.StatusCode != 503 || !bytes.Contains(body, []byte("arca_unavailable")) {
		t.Fatalf("esperado arca_unavailable: %d %s", res.StatusCode, body)
	}
	res, _ = http.Get(ts.URL + "/api/health")
	body, _ = io.ReadAll(res.Body)
	res.Body.Close()
	if !bytes.Contains(body, []byte(`"arca":"down"`)) {
		t.Fatalf("health deveria indicar down: %s", body)
	}
}
