package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"arca/gateway/internal/auth"
	"arca/gateway/internal/resp"
	"arca/gateway/internal/store"
)

func withUser(ctx context.Context, u *store.User) context.Context {
	return context.WithValue(ctx, userKey, u)
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	state := "ok"
	if err := s.client.Ping(ctx); err != nil {
		state = "down"
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "arca": state, "ready": s.Ready()})
}

func (s *Server) handleOverview(w http.ResponseWriter, r *http.Request) {
	raw, err := s.store.Stats(r.Context())
	if err != nil {
		s.fail(w, r, err)
		return
	}
	var stats map[string]any
	if err := json.Unmarshal(raw, &stats); err != nil {
		s.fail(w, r, err)
		return
	}
	tables, err := s.store.UserTables(r.Context())
	if err != nil {
		s.fail(w, r, err)
		return
	}
	stats["tables"] = len(tables)
	writeJSON(w, http.StatusOK, stats)
}

func protectedKey(key string) bool {
	return strings.HasPrefix(key, store.SystemPrefix+":")
}

type scanKey struct {
	Key   string `json:"key"`
	Type  string `json:"type"`
	TTLMs int64  `json:"ttl_ms"`
	Size  int64  `json:"size"`
}

type scanResult struct {
	Cursor string    `json:"cursor"`
	Keys   []scanKey `json:"keys"`
}

func (s *Server) handleKeys(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	match := q.Get("match")
	if match == "" {
		match = "*"
	}
	cursor := q.Get("cursor")
	if cursor == "" {
		cursor = "0"
	}
	count := queryInt(r, "count", 50, 1, 1000)
	var res scanResult
	if err := s.client.JSON(r.Context(), &res, "ARCA.SCANJSON", cursor, match, strconv.Itoa(count)); err != nil {
		s.fail(w, r, err)
		return
	}
	keys := make([]scanKey, 0, len(res.Keys))
	for _, k := range res.Keys {
		if !protectedKey(k.Key) {
			keys = append(keys, k)
		}
	}
	if res.Cursor == "" {
		res.Cursor = "0"
	}
	writeJSON(w, http.StatusOK, scanResult{Cursor: res.Cursor, Keys: keys})
}

func keyParam(w http.ResponseWriter, r *http.Request, key string) (string, bool) {
	if key == "" {
		writeError(w, http.StatusBadRequest, "validation", "Informe a chave.", map[string]string{"key": "obrigatória"})
		return "", false
	}
	if len(key) > 4096 {
		writeError(w, http.StatusBadRequest, "validation", "A chave pode ter no máximo 4096 bytes.", map[string]string{"key": "muito longa"})
		return "", false
	}
	if protectedKey(key) {
		writeError(w, http.StatusForbidden, "forbidden", "Chaves do painel não podem ser acessadas por aqui.", nil)
		return "", false
	}
	return key, true
}

func (s *Server) handleKeyGet(w http.ResponseWriter, r *http.Request) {
	key, ok := keyParam(w, r, r.URL.Query().Get("key"))
	if !ok {
		return
	}
	ctx := r.Context()
	v, err := s.client.Check(ctx, "GET", key)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if v.IsNull() {
		writeError(w, http.StatusNotFound, "not_found", "Chave não encontrada.", nil)
		return
	}
	ttl, err := s.client.Int(ctx, "PTTL", key)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"key":    key,
		"value":  v.Str,
		"ttl_ms": ttl,
		"size":   len(v.Str),
		"binary": !utf8.ValidString(v.Str),
	})
}

type keyPut struct {
	Key   string       `json:"key"`
	Value *string      `json:"value"`
	TTLs  *json.Number `json:"ttl_s"`
}

func ttlSeconds(n *json.Number) (int64, error) {
	if n == nil || n.String() == "" {
		return 0, nil
	}
	v, err := strconv.ParseInt(n.String(), 10, 64)
	if err != nil || v < 0 || v > 10*365*24*3600 {
		return 0, errors.New("TTL inválido")
	}
	return v, nil
}

func (s *Server) handleKeyPut(w http.ResponseWriter, r *http.Request) {
	var req keyPut
	if !decodeBody(w, r, &req) {
		return
	}
	key, ok := keyParam(w, r, req.Key)
	if !ok {
		return
	}
	if req.Value == nil {
		writeError(w, http.StatusBadRequest, "validation", "Informe o valor.", map[string]string{"value": "obrigatório"})
		return
	}
	ttl, err := ttlSeconds(req.TTLs)
	if err != nil {
		writeError(w, http.StatusBadRequest, "validation", "O TTL precisa ser um número inteiro de segundos.", map[string]string{"ttl_s": "inválido"})
		return
	}
	args := []string{"SET", key, *req.Value}
	if ttl > 0 {
		args = append(args, "EX", strconv.FormatInt(ttl, 10))
	}
	if _, err := s.client.Check(r.Context(), args...); err != nil {
		s.fail(w, r, err)
		return
	}
	detail := key
	if ttl > 0 {
		detail += fmt.Sprintf(" (TTL %d s)", ttl)
	}
	s.audit(r, "chave_gravada", detail)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleKeyDelete(w http.ResponseWriter, r *http.Request) {
	key, ok := keyParam(w, r, r.URL.Query().Get("key"))
	if !ok {
		return
	}
	n, err := s.client.Int(r.Context(), "DEL", key)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if n == 0 {
		writeError(w, http.StatusNotFound, "not_found", "Chave não encontrada.", nil)
		return
	}
	s.audit(r, "chave_removida", key)
	w.WriteHeader(http.StatusNoContent)
}

type keyExpire struct {
	Key  string       `json:"key"`
	TTLs *json.Number `json:"ttl_s"`
}

func (s *Server) handleKeyExpire(w http.ResponseWriter, r *http.Request) {
	var req keyExpire
	if !decodeBody(w, r, &req) {
		return
	}
	key, ok := keyParam(w, r, req.Key)
	if !ok {
		return
	}
	ttl, err := ttlSeconds(req.TTLs)
	if err != nil {
		writeError(w, http.StatusBadRequest, "validation", "O TTL precisa ser um número inteiro de segundos.", map[string]string{"ttl_s": "inválido"})
		return
	}
	ctx := r.Context()
	exists, err := s.client.Int(ctx, "EXISTS", key)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if exists == 0 {
		writeError(w, http.StatusNotFound, "not_found", "Chave não encontrada.", nil)
		return
	}
	if ttl > 0 {
		_, err = s.client.Check(ctx, "PEXPIRE", key, strconv.FormatInt(ttl*1000, 10))
	} else {
		_, err = s.client.Check(ctx, "PERSIST", key)
	}
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if ttl > 0 {
		s.audit(r, "ttl_definido", fmt.Sprintf("%s (%d s)", key, ttl))
	} else {
		s.audit(r, "ttl_removido", key)
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleTables(w http.ResponseWriter, r *http.Request) {
	tables, err := s.store.UserTables(r.Context())
	if err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, tables)
}

func (s *Server) userTable(w http.ResponseWriter, r *http.Request) (*store.Table, bool) {
	name := r.PathValue("name")
	if !store.ValidIdent(name) || store.IsSystemTable(name) {
		writeError(w, http.StatusNotFound, "not_found", "Tabela não encontrada.", nil)
		return nil, false
	}
	t, err := s.store.Table(r.Context(), name)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeError(w, http.StatusNotFound, "not_found", "Tabela não encontrada.", nil)
			return nil, false
		}
		s.fail(w, r, err)
		return nil, false
	}
	return t, true
}

func validFilter(filter string) bool {
	if len(filter) > 2000 {
		return false
	}
	inQuote := false
	for _, c := range filter {
		switch {
		case c == '\'':
			inQuote = !inQuote
		case !inQuote && c == ';':
			return false
		}
	}
	lower := strings.ToLower(filter)
	return !strings.Contains(lower, store.SystemPrefix)
}

func (s *Server) handleRows(w http.ResponseWriter, r *http.Request) {
	t, ok := s.userTable(w, r)
	if !ok {
		return
	}
	q := r.URL.Query()
	limit := queryInt(r, "limit", 50, 1, 1000)
	offset := queryInt(r, "offset", 0, 0, 1<<30)
	where := ""
	if filter := strings.TrimSpace(q.Get("filter")); filter != "" {
		if !validFilter(filter) {
			writeError(w, http.StatusBadRequest, "validation", "Filtro inválido.", map[string]string{"filter": "use uma expressão WHERE simples"})
			return
		}
		where = " WHERE " + filter
	}
	order := ""
	if col := q.Get("order"); col != "" {
		c := t.Column(col)
		if c == nil {
			writeError(w, http.StatusBadRequest, "validation", "Coluna de ordenação inexistente.", map[string]string{"order": "coluna desconhecida"})
			return
		}
		dir := "ASC"
		if strings.EqualFold(q.Get("dir"), "desc") {
			dir = "DESC"
		}
		order = " ORDER BY " + c.Name + " " + dir
	}
	ctx := r.Context()
	res, err := s.store.SQLf(ctx, "SELECT * FROM %s%s%s LIMIT %d OFFSET %d", t.Name, where, order, limit, offset)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	total := t.Rows
	if where != "" {
		cnt, err := s.store.SQLf(ctx, "SELECT COUNT(*) FROM %s%s", t.Name, where)
		if err != nil {
			s.fail(w, r, err)
			return
		}
		if len(cnt.Rows) > 0 && len(cnt.Rows[0]) > 0 {
			total = store.CellInt(cnt.Rows[0][0])
		}
	}
	pk := ""
	if c := t.PrimaryKey(); c != nil {
		pk = c.Name
	}
	rows := res.Rows
	if rows == nil {
		rows = [][]any{}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"columns":     res.Columns,
		"types":       res.Types,
		"rows":        rows,
		"total":       total,
		"primary_key": pk,
	})
}

type rowWrite struct {
	Key    any            `json:"key"`
	Values map[string]any `json:"values"`
}

func columnsAndLiterals(t *store.Table, values map[string]any, skipPK bool) ([]string, []string, map[string]string) {
	fields := map[string]string{}
	names := make([]string, 0, len(values))
	for name := range values {
		names = append(names, name)
	}
	sort.Strings(names)
	cols := make([]string, 0, len(names))
	lits := make([]string, 0, len(names))
	for _, name := range names {
		c := t.Column(name)
		if c == nil {
			fields[name] = "coluna inexistente"
			continue
		}
		if skipPK && c.PrimaryKey {
			continue
		}
		lit, err := store.Literal(c.Type, values[name])
		if err != nil {
			fields[c.Name] = err.Error()
			continue
		}
		if lit == "NULL" && c.NotNull && !(c.PrimaryKey && c.AutoIncrement) {
			fields[c.Name] = "não pode ficar vazio"
			continue
		}
		if lit == "NULL" && c.PrimaryKey && c.AutoIncrement {
			continue
		}
		cols = append(cols, c.Name)
		lits = append(lits, lit)
	}
	return cols, lits, fields
}

func (s *Server) handleRowInsert(w http.ResponseWriter, r *http.Request) {
	t, ok := s.userTable(w, r)
	if !ok {
		return
	}
	var req rowWrite
	if !decodeBody(w, r, &req) {
		return
	}
	if len(req.Values) == 0 {
		writeError(w, http.StatusBadRequest, "validation", "Informe ao menos uma coluna.", nil)
		return
	}
	cols, lits, fields := columnsAndLiterals(t, req.Values, false)
	if len(fields) > 0 {
		writeError(w, http.StatusBadRequest, "validation", "Confira os campos destacados.", fields)
		return
	}
	if len(cols) == 0 {
		writeError(w, http.StatusBadRequest, "validation", "Informe ao menos uma coluna.", nil)
		return
	}
	query := fmt.Sprintf("INSERT INTO %s (%s) VALUES (%s)", t.Name, strings.Join(cols, ", "), strings.Join(lits, ", "))
	res, err := s.store.SQL(r.Context(), query)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	s.audit(r, "linha_inserida", t.Name)
	writeJSON(w, http.StatusCreated, map[string]any{"message": res.Tag, "affected": res.Affected})
}

func (s *Server) pkLiteral(w http.ResponseWriter, t *store.Table, key any) (string, *store.Column, bool) {
	pk := t.PrimaryKey()
	if pk == nil {
		writeError(w, http.StatusBadRequest, "validation", "A tabela não tem chave primária.", nil)
		return "", nil, false
	}
	if key == nil {
		writeError(w, http.StatusBadRequest, "validation", "Informe a chave primária.", map[string]string{"key": "obrigatória"})
		return "", nil, false
	}
	lit, err := store.Literal(pk.Type, key)
	if err != nil || lit == "NULL" {
		writeError(w, http.StatusBadRequest, "validation", "Chave primária inválida.", map[string]string{"key": "valor incompatível com " + pk.Type})
		return "", nil, false
	}
	return lit, pk, true
}

func (s *Server) handleRowUpdate(w http.ResponseWriter, r *http.Request) {
	t, ok := s.userTable(w, r)
	if !ok {
		return
	}
	var req rowWrite
	if !decodeBody(w, r, &req) {
		return
	}
	lit, pk, ok := s.pkLiteral(w, t, req.Key)
	if !ok {
		return
	}
	cols, lits, fields := columnsAndLiterals(t, req.Values, true)
	if len(fields) > 0 {
		writeError(w, http.StatusBadRequest, "validation", "Confira os campos destacados.", fields)
		return
	}
	if len(cols) == 0 {
		writeError(w, http.StatusBadRequest, "validation", "Nada para alterar.", nil)
		return
	}
	sets := make([]string, len(cols))
	for i := range cols {
		sets[i] = cols[i] + " = " + lits[i]
	}
	res, err := s.store.SQLf(r.Context(), "UPDATE %s SET %s WHERE %s = %s", t.Name, strings.Join(sets, ", "), pk.Name, lit)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if res.Affected == 0 {
		writeError(w, http.StatusNotFound, "not_found", "Linha não encontrada.", nil)
		return
	}
	s.audit(r, "linha_alterada", fmt.Sprintf("%s %s = %s", t.Name, pk.Name, lit))
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleRowDelete(w http.ResponseWriter, r *http.Request) {
	t, ok := s.userTable(w, r)
	if !ok {
		return
	}
	raw := r.URL.Query().Get("key")
	var key any
	if raw != "" || r.URL.Query().Has("key") {
		key = raw
	}
	lit, pk, ok := s.pkLiteral(w, t, key)
	if !ok {
		return
	}
	res, err := s.store.SQLf(r.Context(), "DELETE FROM %s WHERE %s = %s", t.Name, pk.Name, lit)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if res.Affected == 0 {
		writeError(w, http.StatusNotFound, "not_found", "Linha não encontrada.", nil)
		return
	}
	s.audit(r, "linha_removida", fmt.Sprintf("%s %s = %s", t.Name, pk.Name, lit))
	w.WriteHeader(http.StatusNoContent)
}

type sqlRequest struct {
	Query string `json:"query"`
}

func (s *Server) handleSQL(w http.ResponseWriter, r *http.Request) {
	var req sqlRequest
	if !decodeBody(w, r, &req) {
		return
	}
	query := strings.TrimSpace(req.Query)
	query = strings.TrimSpace(strings.TrimSuffix(query, ";"))
	if query == "" {
		writeError(w, http.StatusBadRequest, "validation", "Escreva uma consulta.", map[string]string{"query": "vazia"})
		return
	}
	if len(query) > 64<<10 {
		writeError(w, http.StatusBadRequest, "validation", "A consulta passa de 64 KB.", nil)
		return
	}
	u := currentUser(r)
	readOnly := store.IsReadOnly(query)
	if !readOnly && !auth.Allows(u.Role, auth.RoleOperator) {
		writeError(w, http.StatusForbidden, "forbidden", "Seu papel permite apenas consultas de leitura (SELECT, EXPLAIN, SHOW e DESCRIBE).", nil)
		return
	}
	if u.Role != auth.RoleAdmin && strings.Contains(strings.ToLower(query), store.SystemPrefix) {
		writeError(w, http.StatusForbidden, "forbidden", "As tabelas internas do painel só podem ser consultadas por administradores.", nil)
		return
	}
	raw, err := s.store.SQLRaw(r.Context(), query)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	if !readOnly {
		s.audit(r, "sql", query)
	}
	writeRaw(w, http.StatusOK, raw)
}

type commandRequest struct {
	Args []string `json:"args"`
}

var longCommands = map[string]bool{"ARCA.COMPACT": true, "ARCA.CHECKPOINT": true, "ARCA.FLUSH": true, "SAVE": true, "BGSAVE": true, "FLUSHALL": true, "FLUSHDB": true, "KEYS": true, "DBSIZE": true}

func (s *Server) handleCommand(w http.ResponseWriter, r *http.Request) {
	var req commandRequest
	if !decodeBody(w, r, &req) {
		return
	}
	if len(req.Args) == 0 || strings.TrimSpace(req.Args[0]) == "" {
		writeError(w, http.StatusBadRequest, "validation", "Informe o comando.", map[string]string{"args": "vazio"})
		return
	}
	name := strings.ToUpper(req.Args[0])
	timeout := 30 * time.Second
	if longCommands[name] {
		timeout = 10 * time.Minute
	}
	start := time.Now()
	v, err := s.client.DoTimeout(r.Context(), timeout, req.Args...)
	if err != nil {
		s.fail(w, r, err)
		return
	}
	ms := float64(time.Since(start).Microseconds()) / 1000
	s.audit(r, "comando", strings.Join(req.Args, " "))
	writeJSON(w, http.StatusOK, map[string]any{"reply": v.ToJSON(), "ms": ms})
}

func (s *Server) handleStorage(w http.ResponseWriter, r *http.Request) {
	raw, err := s.store.Stats(r.Context())
	if err != nil {
		s.fail(w, r, err)
		return
	}
	var stats struct {
		Levels            json.RawMessage `json:"levels"`
		Engine            json.RawMessage `json:"engine"`
		RecentCompactions json.RawMessage `json:"recent_compactions"`
	}
	if err := json.Unmarshal(raw, &stats); err != nil {
		s.fail(w, r, err)
		return
	}
	out := map[string]json.RawMessage{
		"levels":             orEmpty(stats.Levels, "[]"),
		"engine":             orEmpty(stats.Engine, "{}"),
		"recent_compactions": orEmpty(stats.RecentCompactions, "[]"),
	}
	writeJSON(w, http.StatusOK, out)
}

func orEmpty(raw json.RawMessage, fallback string) json.RawMessage {
	if len(raw) == 0 || string(raw) == "null" {
		return json.RawMessage(fallback)
	}
	return raw
}

func (s *Server) storageAction(w http.ResponseWriter, r *http.Request, args []string, action, done string, timeout time.Duration) {
	start := time.Now()
	v, err := s.client.DoTimeout(r.Context(), timeout, args...)
	if err == nil {
		err = v.Err()
	}
	if err != nil {
		s.fail(w, r, err)
		return
	}
	elapsed := time.Since(start).Round(time.Millisecond)
	msg := fmt.Sprintf("%s em %s.", done, elapsed)
	if v.Kind == resp.Bulk && v.Str != "" {
		msg = fmt.Sprintf("%s em %s: %s", done, elapsed, v.Str)
	}
	s.audit(r, action, msg)
	writeJSON(w, http.StatusOK, map[string]any{"message": msg})
}

func (s *Server) handleFlush(w http.ResponseWriter, r *http.Request) {
	s.storageAction(w, r, []string{"ARCA.FLUSH"}, "flush", "Memtable gravada em disco", 2*time.Minute)
}

func (s *Server) handleCompact(w http.ResponseWriter, r *http.Request) {
	s.storageAction(w, r, []string{"ARCA.COMPACT"}, "compactacao", "Compactação concluída", 30*time.Minute)
}

func (s *Server) handleCheckpoint(w http.ResponseWriter, r *http.Request) {
	name := "painel-" + time.Now().UTC().Format("20060102-150405")
	s.storageAction(w, r, []string{"ARCA.CHECKPOINT", name}, "checkpoint", "Checkpoint criado", 10*time.Minute)
}

type clientInfo struct {
	ID    int64  `json:"id"`
	Addr  string `json:"addr"`
	Name  string `json:"name"`
	AgeS  int64  `json:"age_s"`
	IdleS int64  `json:"idle_s"`
	Cmd   string `json:"cmd"`
}

func parseClientList(text string) []clientInfo {
	out := []clientInfo{}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var c clientInfo
		for _, field := range strings.Fields(line) {
			k, v, ok := strings.Cut(field, "=")
			if !ok {
				continue
			}
			switch k {
			case "id":
				c.ID, _ = strconv.ParseInt(v, 10, 64)
			case "addr":
				c.Addr = v
			case "name":
				c.Name = v
			case "age":
				c.AgeS, _ = strconv.ParseInt(v, 10, 64)
			case "idle":
				c.IdleS, _ = strconv.ParseInt(v, 10, 64)
			case "cmd":
				c.Cmd = v
			}
		}
		out = append(out, c)
	}
	return out
}

func (s *Server) handleClients(w http.ResponseWriter, r *http.Request) {
	text, err := s.client.Text(r.Context(), "CLIENT", "LIST")
	if err != nil {
		s.fail(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, parseClientList(text))
}

type slowEntry struct {
	ID     int64    `json:"id"`
	At     int64    `json:"at"`
	Micros int64    `json:"micros"`
	Args   []string `json:"args"`
	Addr   string   `json:"addr"`
}

func (s *Server) handleSlowlog(w http.ResponseWriter, r *http.Request) {
	n := queryInt(r, "n", 50, 1, 1000)
	v, err := s.client.Check(r.Context(), "SLOWLOG", "GET", strconv.Itoa(n))
	if err != nil {
		s.fail(w, r, err)
		return
	}
	out := []slowEntry{}
	for _, item := range v.Array {
		if item.Kind != resp.Array || len(item.Array) < 4 {
			continue
		}
		e := slowEntry{ID: item.Array[0].Int, At: item.Array[1].Int * 1000, Micros: item.Array[2].Int, Args: []string{}}
		for _, a := range item.Array[3].Array {
			e.Args = append(e.Args, a.Text())
		}
		if len(item.Array) > 4 {
			e.Addr = item.Array[4].Text()
		}
		out = append(out, e)
	}
	writeJSON(w, http.StatusOK, out)
}

type configEntry struct {
	Name  string `json:"name"`
	Value string `json:"value"`
}

func (s *Server) handleConfigGet(w http.ResponseWriter, r *http.Request) {
	v, err := s.client.Check(r.Context(), "CONFIG", "GET", "*")
	if err != nil {
		s.fail(w, r, err)
		return
	}
	out := []configEntry{}
	for i := 0; i+1 < len(v.Array); i += 2 {
		name := v.Array[i].Text()
		if strings.Contains(strings.ToLower(name), "pass") {
			continue
		}
		out = append(out, configEntry{Name: name, Value: v.Array[i+1].Text()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	writeJSON(w, http.StatusOK, out)
}

func (s *Server) handleConfigSet(w http.ResponseWriter, r *http.Request) {
	var req configEntry
	if !decodeBody(w, r, &req) {
		return
	}
	name := strings.TrimSpace(req.Name)
	if name == "" {
		writeError(w, http.StatusBadRequest, "validation", "Informe o parâmetro.", map[string]string{"name": "obrigatório"})
		return
	}
	if strings.Contains(strings.ToLower(name), "pass") {
		writeError(w, http.StatusForbidden, "forbidden", "A senha do servidor não pode ser alterada pelo painel.", nil)
		return
	}
	if _, err := s.client.Check(r.Context(), "CONFIG", "SET", name, req.Value); err != nil {
		s.fail(w, r, err)
		return
	}
	s.audit(r, "config", name+" = "+req.Value)
	w.WriteHeader(http.StatusNoContent)
}
