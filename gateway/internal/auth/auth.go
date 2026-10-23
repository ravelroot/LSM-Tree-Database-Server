package auth

import (
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"net/mail"
	"strconv"
	"strings"
	"sync"
	"unicode"
)

const (
	Iterations = 210000
	saltLen    = 16
	keyLen     = 32
	scheme     = "pbkdf2-sha256"
)

const (
	RoleReader   = "reader"
	RoleOperator = "operator"
	RoleAdmin    = "admin"
)

var roleRank = map[string]int{RoleReader: 1, RoleOperator: 2, RoleAdmin: 3}

func ValidRole(role string) bool {
	_, ok := roleRank[role]
	return ok
}

func Allows(have, need string) bool {
	return roleRank[have] >= roleRank[need] && roleRank[need] > 0
}

func HashPassword(password string) (string, error) {
	return hashWith(password, Iterations)
}

func hashWith(password string, iterations int) (string, error) {
	salt := make([]byte, saltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	key, err := pbkdf2.Key(sha256.New, password, salt, iterations, keyLen)
	if err != nil {
		return "", err
	}
	enc := base64.RawStdEncoding
	return fmt.Sprintf("%s$%d$%s$%s", scheme, iterations, enc.EncodeToString(salt), enc.EncodeToString(key)), nil
}

func VerifyPassword(password, encoded string) bool {
	parts := strings.Split(encoded, "$")
	if len(parts) != 4 || parts[0] != scheme {
		return false
	}
	iterations, err := strconv.Atoi(parts[1])
	if err != nil || iterations < 1000 || iterations > 10_000_000 {
		return false
	}
	enc := base64.RawStdEncoding
	salt, err := enc.DecodeString(parts[2])
	if err != nil {
		return false
	}
	want, err := enc.DecodeString(parts[3])
	if err != nil || len(want) == 0 {
		return false
	}
	got, err := pbkdf2.Key(sha256.New, password, salt, iterations, len(want))
	if err != nil {
		return false
	}
	return subtle.ConstantTimeCompare(got, want) == 1
}

var (
	dummyOnce sync.Once
	dummyHash string
)

func BurnTime(password string) {
	dummyOnce.Do(func() {
		dummyHash, _ = HashPassword("arca-senha-inexistente-0")
	})
	VerifyPassword(password, dummyHash)
}

func CheckPolicy(password string) error {
	if len([]rune(password)) < 8 {
		return errors.New("A senha precisa ter pelo menos 8 caracteres.")
	}
	if len(password) > 256 {
		return errors.New("A senha pode ter no máximo 256 caracteres.")
	}
	var letter, digit bool
	for _, r := range password {
		if unicode.IsLetter(r) {
			letter = true
		}
		if unicode.IsDigit(r) {
			digit = true
		}
	}
	if !letter || !digit {
		return errors.New("A senha precisa ter letras e números.")
	}
	return nil
}

func NormalizeEmail(email string) (string, error) {
	email = strings.ToLower(strings.TrimSpace(email))
	if email == "" || len(email) > 254 {
		return "", errors.New("Informe um e-mail válido.")
	}
	addr, err := mail.ParseAddress(email)
	if err != nil || addr.Address != email || !strings.Contains(email[strings.LastIndex(email, "@")+1:], ".") {
		return "", errors.New("Informe um e-mail válido.")
	}
	return email, nil
}

func NewToken() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

func TokenHash(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}
