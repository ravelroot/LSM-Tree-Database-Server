FROM ubuntu:24.04 AS build-cpp
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY tools tools
COPY examples examples
COPY tests tests
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DARCA_BUILD_TESTS=OFF \
 && cmake --build build \
 && strip build/arca-server build/arca-cli build/arca-inspect build/arca-bench

FROM node:22-alpine AS build-web
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web ./
RUN npm run build

FROM golang:1.24-alpine AS build-go
WORKDIR /gateway
COPY gateway ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o /arca-gateway ./cmd/arca-gateway

FROM ubuntu:24.04 AS server
RUN useradd --system --uid 10001 arca && mkdir -p /dados && chown arca /dados
COPY --from=build-cpp /src/build/arca-server /src/build/arca-cli /src/build/arca-inspect /src/build/arca-bench /usr/local/bin/
USER arca
WORKDIR /dados
EXPOSE 7379
VOLUME /dados
ENTRYPOINT ["arca-server"]
CMD ["--bind", "0.0.0.0", "--dir", "/dados/banco"]

FROM alpine:3.20 AS gateway
RUN adduser -D -u 10001 arca
COPY --from=build-go /arca-gateway /usr/local/bin/arca-gateway
COPY --from=build-web /web/dist /srv/painel
ENV ARCA_WEB_DIR=/srv/painel ARCA_HTTP_ADDR=:8080
USER arca
EXPOSE 8080
ENTRYPOINT ["arca-gateway"]
