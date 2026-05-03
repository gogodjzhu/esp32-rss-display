# AGENTS.md

## 项目概览

双组件项目：ESP32-C3 固件 + Go 后端服务器。

- **固件** (`src/`, `lib/`)：PlatformIO + ESP-IDF，负责拉取 RSS 并提供 WiFi 配置门户。
- **后端** (`backend/`)：Go HTTP 服务器，聚合 RSS 订阅源，渲染 320×240 JPEG 图片并推送给设备。

---

## 固件（PlatformIO + ESP-IDF）

**工具链：** PlatformIO，`framework = espidf`，板型 `airm2m_core_esp32c3`，ESP-IDF 6.0.0。

```sh
pio run                          # 编译
pio run --target upload          # 编译并烧录（自动烧录 SPIFFS）
pio device monitor               # 串口监视器（115200 波特率）
pio run --target clean           # 清理
```

**SPIFFS 镜像**由 `build_spiffs.py` 作为 PlatformIO 预编译钩子自动生成，无需手动操作。源文件位于 `data/www/`，烧录偏移地址为 `0x110000`。

**入口：** `src/main.cpp` — `extern "C" void app_main()`。`lib/` 下所有库模块均为纯 C，带 `extern "C"` 守卫。

**`sdkconfig.airm2m_core_esp32c3`** 为自动生成文件，禁止手动编辑。`sdkconfig.defaults` 是已提交的配置基准。

**根目录和 `src/` 下均有 CMakeLists.txt**，支持直接用 `idf.py` 构建，但推荐使用 PlatformIO 工作流。`src/CMakeLists.txt` 使用 `GLOB_RECURSE` 递归收集 `src/` 和 `lib/` 下的源文件。

**注意：** `platformio.ini` 中的 `build_unflags = -fuse-cxa-atexit` 是 ESP-IDF 所需的 C++ ABI 兼容性配置，禁止删除。

**配置**通过 Kconfig（`src/Kconfig`）管理。RSS URL 默认为 `http://192.168.3.107:1200/nytimes`（本地 RSSHub）。在 `sdkconfig.defaults` 中递增 `FIRMWARE_VERSION` 可在下次启动时触发 NVS 清除。当前值为 `CONFIG_FIRMWARE_VERSION=6`。

---

## 后端（Go）

```sh
cd backend
CGO_ENABLED=0 go build -o bin/server ./cmd/server   # 编译（CGO_ENABLED=0 必须设置，使用纯 Go SQLite）
./bin/server --config config.yaml                   # 运行
go test ./...                                       # 测试（暂无测试用例）
go mod tidy                                         # 同步依赖
```

**API 接口：**
- `GET /v1/device/{device_id}/next` — 获取设备的下一条 RSS 内容
- `GET /nfc/{device_id}` — 重定向到当前条目的原始 URL
- `GET /metrics` — Prometheus 监控指标
- `GET /image/{id}.jpg` — 渲染图片（默认从 `data/images/` 目录提供）

**数据库：** 默认使用 SQLite，路径为 `/tmp/data/rss.db`（重启后不保留）。也支持通过 `config.yaml` 配置 `database.driver: mysql` 使用 MySQL。每次启动时执行 GORM AutoMigrate。

**订阅源去重：** `config.yaml` 中的订阅源仅在 URL 不存在时才插入，重复运行不会更新已有记录。如需强制重新插入，删除对应数据库行即可。

**日志注意：** `rss/worker.go` 在 stderr 为 TTY 时会静默 `log` 输出（`log.SetOutput(io.Discard)`）。需将 stderr 重定向或在非 TTY 环境下运行才能看到日志。

---

## 代码规范

- 源文件中的注释统一使用**中文**，固件和后端均遵循此规范。
- `lib/` 下的库代码为**纯 C**；`main.cpp` 是唯一的 C++ 文件。

---

## 测试与 CI

- `test/` 目录作为 PlatformIO 脚手架存在，但尚无测试实现。
- 项目中没有 CI/CD 配置。

---

## TFT显示屏视觉测试（USB摄像头）

树莓派通过USB连接了一个摄像头，**已对准ESP32-C3连接的TFT显示屏**。可以通过拍照主动观察显示屏内容，进行显示效果验证。

### 拍照命令

```sh
v4l2-ctl --device=/dev/video0 \
  --set-fmt-video=width=1280,height=720,pixelformat=MJPG && \
v4l2-ctl --device=/dev/video0 \
  --stream-mmap --stream-count=1 \
  --stream-to=/tmp/opencode/screen_capture.jpg
```

拍摄的照片保存为 `/tmp/opencode/screen_capture.jpg`（1280×720 JPEG）。

### 使用场景

- **烧录固件后**：拍照确认屏幕是否正常显示、有无花屏或黑屏
- **UI调试**：修改显示布局、字体、颜色后，拍照对比实际效果
- **RSS内容验证**：确认后端推送的图片在屏幕上渲染正确
- **回归测试**：对比修改前后的屏幕截图，检查是否引入视觉问题

### 工作流程

1. 烧录固件：`pio run --target upload`
2. 等待设备启动（约3秒）
3. 拍摄屏幕照片（见上方命令）
4. 用 Read 工具读取图片，分析显示内容是否符合预期
5. 若有问题，修改代码后重复上述步骤

---

## OpenSpec 工作流

使用 `openspec` 进行规格驱动开发。规格文件位于 `openspec/specs/<capability>/spec.md`。通过 OpenCode 斜线命令操作：`/opsx-explore`、`/opsx-propose`、`/opsx-apply`、`/opsx-archive`（需要 `.opencode/` 目录下的 `@opencode-ai/plugin` npm 包）。
