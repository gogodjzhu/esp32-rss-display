cd backend && CGO_ENABLED=0 go build -o bin/server ./cmd/server

# 注意：从设备（ESP32）访问必须使用 IP 而非主机名——ESP32 的 getaddrinfo
# 只查询自身配置的 DNS 服务器（路由器），不读取任何 hosts 文件/本机自解析。
# 树莓派上 curl 能通 "turtle" 只是本机 /etc/hosts 的 127.0.1.1 自条目生效，
# 路由器 DNS 并不注册该名字。
#
# 设备标识由固件首次启动时按芯片 MAC 生成（如 esp32s3-001122aabbcc），
# 也可在管理页面显式填写；下面用 dev-demo 作为任意测试标识。
curl http://localhost:8080/v1/device/dev-demo/next
# next 返回的 item_id 即评分使用条目 ID；同一设备对同一条目重复评分覆盖为最新值
curl -X POST -H 'Content-Type: application/json' \
     -d '{"rating":5,"device_id":"dev-demo"}' \
     http://localhost:8080/v1/item/1/rating
curl http://localhost:8080/nfc/dev-demo
curl http://localhost:8080/metrics

# 行情栏：config.yaml 的 stocks.symbols 填腾讯行情编号（sh/sz/bj/hk/us 前缀），
# 后端每 refresh_seconds 秒拉取一次并缓存全部行情；next 的 size 参数指定本次
# 返回条数（缺省 3），后端按设备轮换窗口。name 可覆盖数据源名称。
curl -s 'http://localhost:8080/v1/device/dev-demo/next?size=6' | jq .stocks
