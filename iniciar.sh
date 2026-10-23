#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v docker >/dev/null 2>&1; then
  echo "Instale o Docker (com Docker Compose) e rode de novo: https://docs.docker.com/get-docker/"
  exit 1
fi

if [ ! -f .env ]; then
  cp .env.example .env
  senha=$(head -c 48 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 40)
  sed -i.bak -e "s|^ARCA_PASSWORD=.*|ARCA_PASSWORD=${senha}|" .env
  rm -f .env.bak
  echo "Arquivo .env criado com uma senha aleatória para o servidor Arca."
fi

docker compose up -d --build

porta=$(grep -E '^ARCA_HTTP_PORT=' .env | cut -d= -f2)
porta=${porta:-8080}
printf "Aguardando a Arca subir"
for _ in $(seq 1 120); do
  if curl -fsS "http://localhost:${porta}/api/health" >/dev/null 2>&1; then
    echo
    echo "Pronto!"
    echo "  Painel:   http://localhost:${porta}"
    echo "  Servidor: localhost:7379 (senha no arquivo .env)"
    echo "  Contas:   admin@arca.dev, operador@arca.dev ou leitura@arca.dev, senha Arca@2026"
    exit 0
  fi
  printf "."
  sleep 2
done
echo
echo "A Arca ainda está subindo. Acompanhe com: docker compose logs -f"
