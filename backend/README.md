cd backend && CGO_ENABLED=0 go build -o bin/server ./cmd/server

# 注意：从设备（ESP32）访问必须使用 IP 而非主机名——ESP32 的 getaddrinfo
# 只查询自身配置的 DNS 服务器（路由器），不读取任何 hosts 文件/本机自解析。
# 树莓派上 curl 能通 "turtle" 只是本机 /etc/hosts 的 127.0.1.1 自条目生效，
# 路由器 DNS 并不注册该名字。
curl http://localhost:8080/v1/device/test-device-001/next
curl http://localhost:8080/nfc/test-device-001
curl http://localhost:8080/metrics