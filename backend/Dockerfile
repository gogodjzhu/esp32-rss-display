FROM golang:1.24-alpine AS builder

WORKDIR /src

# 仅拷贝依赖描述文件，利用 Docker 缓存
COPY go.mod go.sum ./
RUN mkdir -p cmd/server && touch cmd/server/main.go
RUN CGO_ENABLED=0 go build -o /dev/null ./cmd/server 2>/dev/null; true

# 拷贝完整源码并编译
COPY . .
RUN CGO_ENABLED=0 go build -o /out/server ./cmd/server

FROM alpine:latest

COPY --from=builder /out/server /usr/local/bin/esp32-rss-server

EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/esp32-rss-server"]
CMD ["--config", "/app/config.yaml", "--image-dir", "/app/data/images"]