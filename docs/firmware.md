# ESP32-S3 固件开发与验收

固件从后端拉取内容和 320×240 JPEG，提供 BOOT 键轮播/评分、WiFi 配网和后端配置。正式构建入口是 PlatformIO；固定使用 Core 6.2.0、espressif32 6.13.0（ESP-IDF 5.5.3）和 LVGL 9.6.0。

## 构建与检查

```sh
pio run
pio run -e esp32s3
CC=gcc python3 tools/run_host_tests.py --sanitize
python3 tools/check_firmware.py
```

`pio` 不在 PATH 时可使用 `~/.platformio/penv/bin/pio`。主机测试需要 GCC、make 和 CMake；脚本也能使用 PlatformIO 安装的 CMake。`--idf-path` 可指定提供 cJSON 源码的 ESP-IDF 目录。固件的生成配置 `sdkconfig.esp32s3` 禁止手工修改；修改 Kconfig 默认值和 `sdkconfig.defaults` 后应重新生成配置并验证。

`tools/check_firmware.py` 检查单一目标、组件清单、资源尺寸，并将生成的 `partitions.bin` 与源 CSV 逐项比较。CI 从空项目构建目录执行默认构建、主机测试和资源检查，并保存烧录镜像；本地写入 CI 文件不代表远端 CI 已运行。

`pio run --target upload` 会烧录固件、SPIFFS 和字体，必须连接设备后由操作者执行。`partitions_s3.csv` 保持原布局：NVS 在 `0x9000`，应用在 `0x10000`，storage 在 `0x210000`（512KiB），font 在 `0x290000`（2MiB）。两个资源钩子共用 IDF 官方分区解析器，并在烧录前检查尺寸。应用分区为 2MiB，板上 flash 为 8MiB。

原生 `idf.py` 的依赖获取、资源构建和完整烧录尚未验收；不能仅凭 CMake 配置存在就将其作为发布路径。LVGL 组件允许通过 `LVGL_DIR` 显式提供源码路径。

## 模块与所有权

| 模块 | 职责与资源 |
| --- | --- |
| `main.cpp` | 进入纯 C 控制器，启动错误时显示错误页 |
| `app_controller` / `app_state` | 当前内容、请求编号、重试、评分和单调时钟 deadline；纯状态逻辑可主机测试 |
| `button_input` | GPIO0 中断消抖、时间戳队列与丢失计数；手势和评分规则在状态机执行 |
| `network_worker` | 唯一网络任务，顺序执行 next、图片下载和评分；不访问屏幕或字体 |
| `backend_client` / `content` | URL、JSON 协议与完整内容模型；评分显式传入条目 ID |
| `http_transport` | 有上限、检查完整性的 GET/POST；同时返回传输错误和 HTTP 状态 |
| `content_view` / `jpeg_decoder` | 在 UI 任务解码并组合图片与标题，逐行输出，无全屏 framebuffer |
| `display_driver` | S3 引脚与 ST7789 SPI；同步写入，调用返回后可复用输入像素 |
| `ui_animation` / `banner` / `cjk_font` | LVGL 页面、底栏、标题字形和动画；全部由 UI 任务调用 |
| `wifi` | 默认事件循环内串行处理连接、重试、主动断开与配网，输出受保护的快照 |
| `app_config` / `nvs_manager` | 设备标识、校验、迁移、完整配置保存、加载状态与降级恢复；业务模块不直接访问 NVS key |
| `http_server` | 有界接收和解析管理请求、静态文件分块发送、调用服务并返回响应 |

`src/CMakeLists.txt` 将控制器和管理 API 放在一个应用组件中，其余组件显式声明源文件和依赖。PlatformIO LDF 已关闭，避免同一源码被重复编译。历史 `image_fetcher` 和 `rss_reader` 已从源码树删除（需要查阅时用 Git 历史），`lib/` 下只剩注册组件；`tools/check_firmware.py` 校验每个注册组件都真正进入构建。`/api/rss` 已移除，当前内容由 `/api/status` 提供。新增库必须注册组件，不得依赖递归收集所有源码。

底部 2px 由进度条、评分条和评分结果条共用一条绘制路径，只有内容变化或该区域被覆盖（整屏刷新、评分条、结果条）后才重写，避免每帧重复相同的 SPI 写入。

应用/UI 任务独占所有 LVGL、字体、标题缓存、JPEG 解码和屏幕操作。网络任务通过结果队列移交 JPEG 缓冲，控制器在渲染、替换候选或丢弃过期结果时释放。每次任务携带 request ID；评分还携带用户操作时的 item ID。评分和反馈期间保留旧内容，下载好的候选内容等待交互结束后再展示。断网保留当前内容，旧 next 结果失效并释放。

输入队列有 16 个时间戳槽，中断消抖 50ms。双击间隔小于 400ms 进入评分，单击窗口到期请求下一条。评分按键间隔至少 200ms，分值 0～5 循环；5 秒无操作后提交，0 分直接退出。评分暂停 60 秒轮播计时，提交反馈停留 1 秒后恢复剩余时间。网络慢请求不会阻塞 UI 事件消费。JPEG 解码仍在 UI 任务同步完成，每约 20ms 让出 CPU，输入要到整图解码完成后才处理；实际最长延迟需要板上测量。

## 配置与后端契约

管理页面使用 `api_base_url`、`device_id` 和 `headers`，由固件生成：

```text
GET  <base>/v1/device/<device_id>/next
POST <base>/v1/item/<item_id>/rating
     {"rating":1,"device_id":"..."}
```

next 响应支持 `title`、`published_at`、`image_url` 和可选 `item_id`。缺少图像时显示标题与纯色背景；存在的 ID 必须是非零 uint32 整数。缺少 ID 的内容仍可正常轮播，评分会返回失败反馈，不发送请求。仓库内 Go 后端已返回稳定 `item_id` 并实现 `/v1/item/{item_id}/rating`：同一设备对同一条目重复评分覆盖为最新值，评分不移动设备的 `current_item_id`，也不改变轮播进度；`server/api/rating_test.go` 覆盖落库、覆盖写、非法输入和轮播隔离。对外部后端的要求相同。

配置使用 schema 1，以 `app_config/config_v1` 单个 NVS 字符串存储，检查 write/commit 错误。固件版本只用于发布展示，不再清空用户配置。`active` 是启动时的生效快照，`saved` 是最新持久化快照；管理页面保存后响应完成，再由应用安排重启。

设备标识只在一个来源生成一次：管理页面或旧 next URL 给出显式标识时原样使用、不会被固件改写；没有任何已保存或旧配置时，由芯片 MAC 生成 `esp32s3-<12 位十六进制>` 并随首次配置保存，因此多台设备不会共享同一个测试身份。`CONFIG_DEVICE_ID` 留空即启用该规则，填写时作为编译期显式覆盖。

没有新结构配置时读取旧 WiFi、HTTP headers 和 backend URL：标准完整 next 地址转换为根地址/前缀及 device ID；根地址直接迁移；不能可靠识别的合法 HTTP 地址完整保留为 `legacy_next_url`，仍可用于 next，设置页提示重新配置，评分不可用。旧键暂时保留，新配置存在后不再读取它们；清除 WiFi 也不会重新导入旧凭证。

启动结果由 `app_config_get_status()` 给出，并在 `/api/status` 的 `config_state` 暴露：`loaded`（读取已保存配置）、`created`（首次生成并保存）、`migrated`（从旧键迁移并保存）、`saved`（本次运行显式保存成功，需重启）、`invalid_retained`（已保存内容损坏、版本不支持或旧值不合法）、`read_failed`（底层读取失败）、`legacy_invalid`（旧地址无法识别）、`storage_unavailable`（存储初始化或写入失败）。后四项为降级状态：原始 NVS 值一律保留，使用无 WiFi 凭证的默认配置开放配网入口，只有显式保存才覆盖；`legacy_invalid` 还会在状态接口给出 `legacy_invalid_url` 原文供诊断。存储不可用时保存如实返回失败，不会报告成功。因此除内存分配、互斥量或标识生成失败外，配置问题不再把设备停在错误画面。

配置 JSON、协议解析和响应生成使用 cJSON。表单和 JSON 请求完整接收后才校验；不记录 WiFi 密码或自定义 header 值。API headers 只附加到 API 请求，图片下载不携带它们。当前拒绝所有重定向，3xx 会产生可观测错误或图片降级；需要鉴权的图片和依赖重定向的图片源需要后续明确支持策略。

## 故障、预算与诊断

- next JSON 上限 4096 字节，超时 10 秒；图片上限 32768 字节，超时 15 秒；评分超时 5 秒。
- 图片必须为解码器支持的 320×240 JPEG；尺寸错误、截断、解码失败会显示纯色与标题，并报告独立的降级结果。
- 网络任务的 job/result 队列各 2 个槽；不实现中途取消，通过超时和请求失效处理过期结果。
- LVGL 使用两块 320×20 RGB565 分块缓冲（合计 25600 字节）；标题背景缓冲 320×72 RGB565（46080 字节）。字体文件 mmap，但字形缓存和绘制缓冲占 RAM。
- LVGL 9.6 使用 `LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB`，字体和 LVGL 动态分配进入 ESP-IDF 堆。静态 RAM 的减少不能解释为总运行内存等量减少，不假设存在 PSRAM。
- UI 主任务和网络任务栈各 8192 字节，管理 HTTP 栈 8192 字节；实际余量由状态接口查看。
- WiFi 单次连接超时使用 `WIFI_CONNECT_TIMEOUT_SEC`，退避从 1 秒增长到最大 32 秒，加 0～499ms 抖动。持续离线超过配置超时开启 APSTA 配网入口并继续重试。成功联网后保留救援 AP 直到重启；无凭证时仅开启 AP。

`GET /api/status` 提供设备标识、生效后端地址、配置状态（`config_state`/`config_state_code`/`config_degraded`，降级时附 `legacy_invalid_url`）、当前内容、视图、网络忙状态、最近错误/HTTP 状态、失败计数、最近成功展示时间、WiFi 断开原因/重试数、空闲堆/历史最低堆/最大连续块、UI/网络栈余量和按键丢失数。`last_success_ms` 使用启动后的单调毫秒；不是墙上时间。错误会在后续成功操作时更新，故障排查时应同时观察内容降级类型和计数。

## 验收记录与板级步骤

主机自动化覆盖虚拟时间交互、配置迁移/原子保存失败、saved/active 隔离、JSON 边界和受控 HTTP 客户端故障。传输测试使用 ESP-IDF 接口替身，执行实际 `http_transport`/`backend_client` 实现，不能证明真实 TCP/TLS、WiFi 或驱动行为。

2026-10-28 首轮本地验证通过显式 S3 构建，以及无项目 `.pio` 缓存副本中的默认构建；副本重新安装了 LVGL，平台和工具链使用已安装包。静态 RAM 为 127536/327680 字节，Flash 为 1332949/2097152 字节。8 组 CTest 在 ASan/UBSan 下通过；生成分区、资源尺寸和源码目标扫描通过。

2026-10-28 第二轮（配置恢复、设备标识与历史路径清理）：本地静态 RAM 为 127792/327680 字节，Flash 为 1334305/2097152 字节。主机测试扩展到 11 组，覆盖出厂设备标识生成、配置降级状态（损坏保留、非法旧值、非法旧地址、读取失败、存储不可用）、显式保存恢复与保存失败不覆盖；Go 后端 `go build`、`go vet`、`go test ./...` 通过，评分测试修复命名冲突并新增“评分不推进设备条目”用例；固件默认构建、`tools/check_firmware.py` 与管理页面全部脚本解析通过。本轮仍未烧录设备，硬件与长时间项继续待执行。

板上应执行以下验收，并记录 `/api/status` 趋势：

1. 首次无凭证启动，AP 页面可访问，记录自动生成的设备标识并确认重启后不变；保存 WiFi 和后端后重启，配置正确生效；升级固件保留凭证。
2. 写入损坏的 `config_v1`、非法旧地址和非法旧 headers 后启动，确认进入配网入口、`/api/status` 的 `config_state` 与原始保留值未被清除，并确认显式保存后恢复正常。
3. 正常轮播、单击/双击边界、0 分退出、评分失败与恢复、评分期间候选内容等待。
4. 关闭/恢复路由器、错误密码、DHCP 延迟、手动重连，确认退避与救援 AP；当前画面保留。
5. 使用受控后端模拟慢响应、503、截断、分块响应、超限 JPEG、尺寸错误、损坏图片及评分失败，核对画面和诊断。
6. 模拟字体资源不可用，验证默认字体降级和标题滚动，不将其误认为中文覆盖完整。
7. 连续轮播至少 24 小时，周期记录最小堆、最大连续块、栈余量、失败数和丢键数；重复断网恢复并比较趋势。

本次没有烧录设备，板级及长时间项目均待执行。编译和主机测试通过仅作为进入硬件验收的前提。
