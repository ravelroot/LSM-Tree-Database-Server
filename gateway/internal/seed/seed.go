package seed

import (
	"context"
	"encoding/json"
	"fmt"
	"math/rand/v2"
	"strconv"
	"strings"
	"time"

	"arca/gateway/internal/auth"
	"arca/gateway/internal/store"
)

const DemoPassword = "Arca@2026"

type demoUser struct {
	email, name, role string
}

var demoUsers = []demoUser{
	{"admin@arca.dev", "Administrador de demonstração", auth.RoleAdmin},
	{"operador@arca.dev", "Operador de demonstração", auth.RoleOperator},
	{"leitura@arca.dev", "Leitura de demonstração", auth.RoleReader},
}

var (
	firstNames = []string{"Ana", "Bruno", "Camila", "Davi", "Elisa", "Fábio", "Gabriela", "Heitor", "Isabela", "Joaquim", "Larissa", "Marcelo", "Natália", "Otávio", "Paula", "Rafael", "Sofia", "Tiago", "Vitória", "Yasmin"}
	lastNames  = []string{"Almeida", "Barros", "Cardoso", "Duarte", "Esteves", "Farias", "Gomes", "Lima", "Moraes", "Nogueira", "Pereira", "Queiroz", "Ribeiro", "Santos", "Teixeira", "Vieira"}
	cities     = []string{"São Paulo", "Rio de Janeiro", "Belo Horizonte", "Curitiba", "Porto Alegre", "Recife", "Salvador", "Fortaleza", "Florianópolis", "Manaus", "Goiânia", "Belém"}
	statuses   = []string{"novo", "pago", "enviado", "entregue", "entregue", "entregue", "cancelado"}
	catalog    = map[string][]string{
		"Papelaria":   {"Caderno pautado 96 folhas", "Caneta gel azul", "Marca-texto amarelo", "Bloco de notas adesivas", "Agenda semanal"},
		"Eletrônicos": {"Mouse sem fio", "Teclado mecânico", "Fone de ouvido", "Carregador USB-C 30 W", "Hub USB 4 portas", "Webcam Full HD"},
		"Casa":        {"Luminária de mesa", "Jogo de toalhas", "Cesto organizador", "Vaso de cerâmica", "Relógio de parede"},
		"Livros":      {"Estruturas de Dados", "Sistemas Distribuídos", "Bancos de Dados na Prática", "Redes de Computadores", "Algoritmos em C++"},
		"Esporte":     {"Bola de futebol", "Tapete de yoga", "Garrafa térmica 1 L", "Corda de pular", "Luvas de academia"},
		"Cozinha":     {"Frigideira antiaderente", "Jogo de facas", "Cafeteira italiana", "Pote hermético 1 L", "Tábua de corte"},
	}
	categories = []string{"Papelaria", "Eletrônicos", "Casa", "Livros", "Esporte", "Cozinha"}
)

type Seeder struct {
	Store *store.Store
	Now   func() time.Time
}

func (s *Seeder) Run(ctx context.Context) (bool, error) {
	n, err := s.Store.CountUsers(ctx)
	if err != nil {
		return false, err
	}
	if n > 0 {
		return false, nil
	}
	for _, u := range demoUsers {
		if err := s.Store.CreateUser(ctx, u.email, u.name, u.role, DemoPassword); err != nil {
			return false, fmt.Errorf("conta %s: %w", u.email, err)
		}
	}
	if err := s.sampleData(ctx); err != nil {
		return true, err
	}
	return true, nil
}

func (s *Seeder) sampleData(ctx context.Context) error {
	tables, err := s.Store.Tables(ctx)
	if err != nil {
		return err
	}
	for _, t := range tables {
		if t.Name == "clientes" || t.Name == "produtos" || t.Name == "pedidos" {
			return s.sampleKeys(ctx)
		}
	}
	ddl := []string{
		"CREATE TABLE IF NOT EXISTS clientes (id INT PRIMARY KEY AUTOINCREMENT, nome TEXT NOT NULL, email TEXT, cidade TEXT, ativo BOOL DEFAULT TRUE, criado_em INT)",
		"CREATE TABLE IF NOT EXISTS produtos (id INT PRIMARY KEY AUTOINCREMENT, sku TEXT NOT NULL, nome TEXT NOT NULL, categoria TEXT, preco REAL, estoque INT DEFAULT 0)",
		"CREATE TABLE IF NOT EXISTS pedidos (id INT PRIMARY KEY AUTOINCREMENT, cliente_id INT NOT NULL, produto_id INT NOT NULL, quantidade INT, total REAL, status TEXT, criado_em INT)",
		"CREATE INDEX IF NOT EXISTS idx_clientes_cidade ON clientes (cidade)",
		"CREATE INDEX IF NOT EXISTS idx_produtos_categoria ON produtos (categoria)",
		"CREATE INDEX IF NOT EXISTS idx_pedidos_cliente ON pedidos (cliente_id)",
		"CREATE INDEX IF NOT EXISTS idx_pedidos_status ON pedidos (status)",
	}
	for _, q := range ddl {
		if _, err := s.Store.SQL(ctx, q); err != nil {
			return fmt.Errorf("%s: %w", q, err)
		}
	}
	rng := rand.New(rand.NewPCG(2026, 7379))
	now := s.Now()
	day := int64(24 * time.Hour / time.Millisecond)

	const clientCount = 150
	var rows []string
	for i := 1; i <= clientCount; i++ {
		name := firstNames[rng.IntN(len(firstNames))] + " " + lastNames[rng.IntN(len(lastNames))]
		created := now.UnixMilli() - int64(rng.IntN(365))*day - int64(rng.IntN(86400000))
		active := store.BoolLiteral(rng.IntN(10) != 0)
		rows = append(rows, fmt.Sprintf("(%s, %s, %s, %s, %d)",
			store.Quote(name), store.Quote(fmt.Sprintf("cliente%03d@exemplo.arca.dev", i)), store.Quote(cities[rng.IntN(len(cities))]), active, created))
	}
	if err := s.insertBatches(ctx, "clientes (nome, email, cidade, ativo, criado_em)", rows); err != nil {
		return err
	}

	type product struct {
		price float64
	}
	var products []product
	rows = rows[:0]
	sku := 1
	for _, cat := range categories {
		for _, name := range catalog[cat] {
			price := float64(rng.IntN(48000)+990) / 100
			products = append(products, product{price: price})
			lit, _ := store.RealLiteral(price)
			rows = append(rows, fmt.Sprintf("(%s, %s, %s, %s, %d)",
				store.Quote(fmt.Sprintf("SKU-%04d", sku)), store.Quote(name), store.Quote(cat), lit, rng.IntN(300)))
			sku++
		}
	}
	if err := s.insertBatches(ctx, "produtos (sku, nome, categoria, preco, estoque)", rows); err != nil {
		return err
	}

	rows = rows[:0]
	for i := 0; i < 420; i++ {
		pid := rng.IntN(len(products)) + 1
		qty := rng.IntN(4) + 1
		total := float64(int64(products[pid-1].price*float64(qty)*100+0.5)) / 100
		lit, _ := store.RealLiteral(total)
		created := now.UnixMilli() - int64(rng.IntN(60))*day - int64(rng.IntN(86400000))
		rows = append(rows, fmt.Sprintf("(%d, %d, %d, %s, %s, %d)",
			rng.IntN(clientCount)+1, pid, qty, lit, store.Quote(statuses[rng.IntN(len(statuses))]), created))
	}
	if err := s.insertBatches(ctx, "pedidos (cliente_id, produto_id, quantidade, total, status, criado_em)", rows); err != nil {
		return err
	}
	return s.sampleKeys(ctx)
}

func (s *Seeder) insertBatches(ctx context.Context, target string, rows []string) error {
	const batch = 50
	for i := 0; i < len(rows); i += batch {
		end := min(i+batch, len(rows))
		q := "INSERT INTO " + target + " VALUES " + strings.Join(rows[i:end], ", ")
		if _, err := s.Store.SQL(ctx, q); err != nil {
			return fmt.Errorf("carga de %s: %w", strings.Fields(target)[0], err)
		}
	}
	return nil
}

func (s *Seeder) sampleKeys(ctx context.Context) error {
	rng := rand.New(rand.NewPCG(7379, 2026))
	c := s.Store.Client
	set := func(args ...string) error {
		_, err := c.Check(ctx, append([]string{"SET"}, args...)...)
		return err
	}
	for i := 0; i < 25; i++ {
		id := fmt.Sprintf("%08x", rng.Uint32())
		payload, _ := json.Marshal(map[string]any{"cliente_id": rng.IntN(150) + 1, "itens_no_carrinho": rng.IntN(6), "pagina": "/produtos"})
		if err := set("sessao:"+id, string(payload), "EX", strconv.Itoa(1800+rng.IntN(5400))); err != nil {
			return err
		}
	}
	for i := 1; i <= 32; i++ {
		payload, _ := json.Marshal(map[string]any{"id": i, "estoque": rng.IntN(300), "atualizado_em": s.Now().UnixMilli()})
		args := []string{"cache:produto:" + strconv.Itoa(i), string(payload)}
		if i%3 != 0 {
			args = append(args, "EX", strconv.Itoa(600+rng.IntN(3000)))
		}
		if err := set(args...); err != nil {
			return err
		}
	}
	today := s.Now()
	for d := 0; d < 14; d++ {
		key := "contador:visitas:" + today.AddDate(0, 0, -d).Format("2006-01-02")
		if err := set(key, strconv.Itoa(800+rng.IntN(2400))); err != nil {
			return err
		}
	}
	extra := [][2]string{
		{"contador:pedidos", "420"},
		{"contador:clientes", "150"},
		{"config:frete:gratis_acima", "199.90"},
		{"config:moeda", "BRL"},
		{"config:tema", "claro"},
		{"config:manutencao", "false"},
		{"fila:emails:pendentes", "12"},
	}
	for _, kv := range extra {
		if err := set(kv[0], kv[1]); err != nil {
			return err
		}
	}
	return nil
}
