package auth

import "testing"

func TestPasswordHash(t *testing.T) {
	h, err := hashWith("Segredo123", 2000)
	if err != nil {
		t.Fatal(err)
	}
	if !VerifyPassword("Segredo123", h) || VerifyPassword("segredo123", h) {
		t.Fatal("verificação de senha incorreta")
	}
	h2, _ := hashWith("Segredo123", 2000)
	if h == h2 {
		t.Fatal("sal não está sendo sorteado")
	}
	for _, bad := range []string{"", "md5$1$a$b", "pbkdf2-sha256$abc$a$b", "pbkdf2-sha256$2000$@@$b"} {
		if VerifyPassword("x", bad) {
			t.Fatalf("hash inválido aceito: %q", bad)
		}
	}
}

func TestPolicyAndEmail(t *testing.T) {
	for pw, ok := range map[string]bool{"Arca@2026": true, "abcdefgh": false, "12345678": false, "a1": false, "senha forte 9": true} {
		if (CheckPolicy(pw) == nil) != ok {
			t.Fatalf("política para %q", pw)
		}
	}
	if e, err := NormalizeEmail("  Admin@Arca.DEV "); err != nil || e != "admin@arca.dev" {
		t.Fatalf("e-mail normalizado errado: %q %v", e, err)
	}
	for _, bad := range []string{"semarroba", "a@b", "Nome <a@b.com>", ""} {
		if _, err := NormalizeEmail(bad); err == nil {
			t.Fatalf("e-mail inválido aceito: %q", bad)
		}
	}
	if !Allows(RoleAdmin, RoleOperator) || Allows(RoleReader, RoleOperator) || Allows("x", RoleReader) || Allows(RoleAdmin, "x") {
		t.Fatal("hierarquia de papéis errada")
	}
	a, _ := NewToken()
	b, _ := NewToken()
	if a == b || len(a) < 40 || TokenHash(a) == a || len(TokenHash(a)) != 64 {
		t.Fatal("token fraco")
	}
}
