# AGENTS.md

## 项目概览

双组件项目：ESP32-S3 固件 + Go 后端服务器。

- **固件** (`src/`, `lib/`)：PlatformIO + ESP-IDF，从后端获取内容与 JPEG，并提供 WiFi/后端配置门户。
- **后端** (`backend/`)：Go HTTP 服务器，聚合 RSS 订阅源，渲染 320×240 JPEG 图片并推送给设备。

---

## 固件（PlatformIO + ESP-IDF）

**工具链：** PlatformIO Core 6.2.0，`espressif32 @ 6.13.0`，`framework = espidf`（ESP-IDF 5.5.3），板型 `esp32-s3-devkitc-1`，LVGL 9.6.0。唯一环境为 `esp32s3`，8MB flash，不依赖 PSRAM。

```sh
pio run                          # 编译
pio run --target upload          # 编译并烧录（自动烧录 SPIFFS）
pio device monitor               # 串口监视器（115200 波特率）
pio run --target clean           # 清理
```

**资源镜像：** `build_spiffs.py` 从 `data/www/` 生成 SPIFFS；`build_font.py` 注册 `assets/fonts/cjk_subset.ttf`。两者经 `tools/firmware_assets.py` 使用 IDF 官方分区解析器获取大小和地址，并校验资源尺寸。普通 upload 会同时烧录资源。

**分区表：** 唯一基准为 `partitions_s3.csv`（8MB）。`storage` 偏移 `0x210000`、大小 `0x80000`；`font` 偏移 `0x290000`、大小 `0x200000`。保留原有 NVS 和应用分区布局，修改固件版本不会清除用户凭证。

**中文显示：** 运行期由 `lib/cjk_font` 用 `esp_partition_mmap` 零拷贝映射 `font` 分区里的子集 TTF，交给 LVGL 内置的 Tiny TTF（`src/lv_conf.h` 中 `LV_USE_TINY_TTF=1`，stb_truetype）按需光栅化。字体覆盖 GB2312 全部汉字 + 拉丁 + 常用标点，由 `tools/gen_cjk_font.py`（fonttools）从文泉驿微米黑（Apache-2.0）裁剪，约 1.4MB。重新生成见 `assets/fonts/README.md`。

**入口：** `src/main.cpp` — `extern "C" void app_main()`。`lib/` 下所有库模块均为纯 C，带 `extern "C"` 守卫。

**显示预算：** `lib/ui_limits/ui_limits.h` 是唯一来源，按内容展示时长和行情滚动节奏推导行情条数，并同步 JSON 解析上限、banner 字型表和 next 的 `size` 参数；当前为 20 条。需要省 RAM 时只改 `UI_STOCK_LIMIT`。

**`sdkconfig.esp32s3`** 为自动生成文件，禁止手动编辑。`sdkconfig.defaults` 是已提交的配置基准。

**构建边界：** `src/CMakeLists.txt` 显式注册入口、应用控制器和管理 API，`lib/*/CMakeLists.txt` 注册底层组件。关闭 PlatformIO LDF，避免重复编译；历史 `image_fetcher`、`rss_reader` 已删除，`lib/` 下只剩注册组件，`tools/check_firmware.py` 校验每个组件都真正进入构建。LVGL 位于唯一环境的 `.pio/libdeps/esp32s3/lvgl`，并校验版本。正式支持 PlatformIO 工作流；原生 `idf.py` 的资源生成和烧录尚未验收。

**运行所有权：** `app_controller` 是应用/UI 任务，独占 LVGL、字体、banner、解码及屏幕写入。`network_worker` 只返回数据，JPEG 缓冲通过结果队列移交，UI 渲染或丢弃后释放。所有请求携带 request ID；评分使用当前展示条目的显式 item ID。WiFi 操作由默认事件循环串行执行，对外只读快照。详见 `docs/firmware.md`。

**注意：** `platformio.ini` 中的 `build_unflags = -fuse-cxa-atexit` 是 ESP-IDF 所需的 C++ ABI 兼容性配置，禁止删除。

**TFT（ST7789）接线：** MOSI=GPIO2、SCLK=GPIO1、CS=GPIO6、DC=GPIO5、RST=GPIO4；交互使用板载 BOOT 键（GPIO0，内部上拉，下降沿中断）。板级定义集中于 `lib/display_driver/board_config.h`。

**配置：** Kconfig (`src/Kconfig`) 提供默认后端根地址；`CONFIG_DEVICE_ID` 留空时由芯片 MAC 生成唯一设备标识（`esp32s3-<12位hex>`），填写后作为显式覆盖。`app_config` 集中校验、迁移和保存用户配置，以 `config_v1` 单个 NVS 值承载 schema 1。管理页面保存后重启生效，active/saved 快照分开。固件版本 `CONFIG_FIRMWARE_VERSION=6` 仅作发布标识。已有完整 next URL 可识别则迁移，其他合法旧地址保留并在设置页提示修正；新结构配置损坏、旧配置非法或底层读取失败时一律保留原值、以默认无凭证配置开放配网入口，只有显式保存才覆盖，状态通过 `app_config_get_status()` 与 `/api/status` 的 `config_state` 暴露。

---

## 后端（Go）

```sh
cd backend
CGO_ENABLED=0 go build -o bin/server ./cmd/server   # 编译（CGO_ENABLED=0 必须设置，使用纯 Go SQLite）
./bin/server --config config.yaml                   # 运行
go test ./...                                       # 测试（评分路由与轮播隔离）
go mod tidy                                         # 同步依赖
```

**API 接口：**
- `GET /v1/device/{device_id}/next?size=N` — 获取设备的下一条 RSS 内容（返回稳定 `item_id`，并按 `size` 附带轮换的实时行情 `stocks`，缺省 3 条）
- `POST /v1/item/{item_id}/rating` — 评分落库，同设备同条目覆盖为最新值，不推进 `current_item_id`
- `GET /nfc/{device_id}` — 重定向到当前条目的原始 URL
- `GET /metrics` — Prometheus 监控指标
- `GET /image/{id}.jpg` — 渲染图片（默认从 `data/images/` 目录提供）

**行情：** `config.yaml` 的 `stocks.symbols` 使用腾讯行情编号（`sh`/`sz`/`bj`/`hk`/`us` 前缀），后台按 `refresh_seconds`（默认 30 秒，最小 5 秒）拉取并缓存全部行情；next 按 `size`（缺省 3，最大 32）返回该设备的轮换窗口，拉取失败保留上一份，未配置或无数据时省略 `stocks` 字段。

**数据库：** 默认使用 SQLite，路径为 `/tmp/data/rss.db`（重启后不保留）。也支持通过 `config.yaml` 配置 `database.driver: mysql` 使用 MySQL。每次启动时执行 GORM AutoMigrate。

**订阅源去重：** `config.yaml` 中的订阅源仅在 URL 不存在时才插入，重复运行不会更新已有记录。如需强制重新插入，删除对应数据库行即可。

**日志注意：** `rss/worker.go` 在 stderr 为 TTY 时会静默 `log` 输出（`log.SetOutput(io.Discard)`）。需将 stderr 重定向或在非 TTY 环境下运行才能看到日志。

---

## 代码规范

- 源文件中的注释统一使用**中文**，固件和后端均遵循此规范。
- `lib/` 下的库代码为**纯 C**；`main.cpp` 是唯一的 C++ 文件。

---

## 测试与 CI

- `CC=gcc python3 tools/run_host_tests.py --sanitize`：状态机、配置模型/迁移/保存失败、内容协议与传输故障测试；需要先安装固件依赖以提供同版 cJSON。
- `python3 tools/check_firmware.py`：单目标约束、分区与构建资源检查。
- `.github/workflows/firmware.yml`：仅构建 S3，运行主机测试与资源检查。
- 板级和长时间测试步骤见 `docs/firmware.md`；主机测试不能替代硬件验收。

---