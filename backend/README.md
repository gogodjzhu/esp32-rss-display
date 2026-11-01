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

# 订阅源管理：config.yaml 只做首次种子，日常增删改走接口（下一轮抓取生效）
curl -s http://localhost:8080/v1/feeds
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"name":"HN","url":"https://news.ycombinator.com/rss"}' \
     http://localhost:8080/v1/feeds
curl -s -X PATCH -H 'Content-Type: application/json' \
     -d '{"enabled":false}' http://localhost:8080/v1/feeds/1
curl -s -X DELETE http://localhost:8080/v1/feeds/1

# 封面：条目无图或图片下载失败时，/image/{id}.jpg 依次回退到订阅源自定义封面、
# 按订阅源名称生成的封面，因此 next 始终返回 image_url。
curl -s -X POST -F 'image=@cover.jpg' http://localhost:8080/v1/feeds/1/cover
curl -s -X DELETE http://localhost:8080/v1/feeds/1/cover
curl -s -o /tmp/cover.jpg http://localhost:8080/image/feed/1.jpg

# 运行状态：每个源最近成功/失败、今日条数，条目与设备规模、图片目录占用
curl -s http://localhost:8080/admin/status | jq .

# 单个订阅源的条目（管理页抽屉用）：按发布时间倒序，分页返回
curl -s 'http://localhost:8080/admin/feeds/1/items?limit=20&offset=0' | jq .

# 新增订阅源前的预览校验：真实抓取一次，确认 URL 是可用的 RSS/Atom
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"url":"https://news.ycombinator.com/rss"}' \
     http://localhost:8080/admin/feeds/preview | jq .

# 管理页面：浏览器打开 http://localhost:8080/admin/
# - 「概览」默认打开：条目/设备/评分/存储规模、近 14 天新增与评分分布图表、异常源摘要
# - 「订阅源」表格：状态、最近成功、最近新内容、条目数、展示/NFC/评分、最近错误；
#   操作收进下拉菜单，点击行打开该源已抓取条目的抽屉
# - 新增订阅源必须先「校验并预览」，后端同时会在创建时再校验一次
# - 支持亮色/暗色/跟随系统主题，移动端表格横向滚动、抽屉全屏
# - 前端源码在 backend/admin-web/（Vite + React + Ant Design + Recharts），
#   构建产物 server/admin/static/ 已入库；改前端后执行：
#       cd backend/admin-web && npm install && npm run build
# - 静态资源通过 go:embed 打进二进制，构建与部署不需要额外文件
# - 应用内不做鉴权：生产环境由 Caddy 对 /admin/* 与 /v1/feeds* 加 basic_auth
# 订阅源改动在下一轮抓取生效；config.yaml 只做首次种子，不再覆盖已有源的名称与启用状态

# 轮播与保留策略
# - next 不再按 id 递增游标取内容，而是按 device_item_views 的曝光记录：
#   未看过的条目优先（发布时间倒序），全部看过之后取最久未展示的一条。
# - rss.max_age_hours（默认 72）之外的内容不参与轮播，并由每日清理任务删除；
#   清理同时回收没有任何条目引用的图片文件。
# - 图片存相对路径（按内容哈希命名），同一张图只落一份。
