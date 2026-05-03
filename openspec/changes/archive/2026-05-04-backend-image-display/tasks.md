# Tasks: backend-image-display

## Task 1: Kconfig 新增后端配置项 ✅

- 在 `src/Kconfig` 的 `menu "RSS Display Configuration"` 内追加：
  - `CONFIG_BACKEND_URL`（string，默认 `http://192.168.3.106:8080`）
  - `CONFIG_DEVICE_ID`（string，默认 `test-device-001`）

## Task 2: ui_animation 新增接口 ✅

在 `lib/ui_animation/ui_animation.h` 声明，`ui_animation.c` 实现：

- `ui_animation_show_no_network(void)`
- `ui_animation_update_bottom_bar(int percent)` — 底部 2px 进度条（y=238），不清空图片区域
- `ui_animation_write_row(int x, int y, const uint16_t *rgb565, int width)` — 绕过 LVGL 直接 SPI 写屏，支持 x 偏移
- `ui_animation_prepare_image_mode(void)` — 清除 LVGL 组件，预建底部进度条

## Task 3: 创建 lib/image_fetcher/ 模块 ✅

新建 `lib/image_fetcher/image_fetcher.h` 和 `image_fetcher.c`：

- `image_fetcher_init(void)` — 无状态初始化
- `image_fetcher_get_next_url(char *url_buf, size_t len)` — HTTP GET + 字符串解析 image_url
- `image_fetcher_download_and_show(const char *url)` — 下载 JPEG + TJpgDec 解码 + 全屏写入（320×240）

实现说明：
- JSON 解析使用 `strstr` 字符串搜索（ESP-IDF 6.0 无内置 cJSON）
- JPEG 解码使用 ROM 内置 TJpgDec（`esp32c3/rom/tjpgd.h`），工作内存池 5KB
- JPEG buffer 上限 80KB，动态 malloc/free

## Task 4: 重构 main.cpp ✅

- WiFi 连接失败 → `ui_animation_show_no_network()` → 挂起
- 连接成功 → `xTaskCreate(image_display_task, 8192)`
- `image_display_task`：拉取 URL → `prepare_image_mode()` → 下载显示 → 进度条倒计时 30s

## Task 5: 编译验证 ✅

- `pio run` 零错误（RAM 41.8%，Flash 57.8%）

## Task 6: 烧录并拍照验证 ✅

- 设备正常连接 WiFi，拉取后端图片并全屏显示
- 修复 bug：`JPEG_POOL_SIZE` 3KB→5KB（`jd_prepare` JDR_FMT3 错误）
- 修复 bug：`write_row` 加 `x` 参数，`outfunc` 传 `rect->left`（图片只写左侧一条）
- 修复 bug：`prepare_image_mode` 清除 spinner（spinner 遮挡图片）

## Task 7: 进度条 UI 优化 ✅

- 进度条高度从 20px → 2px，位置从 y=220 → y=238
- 图片区域从 220 行扩展至全屏 240 行（`IMAGE_DISP_ROWS` 240）
- 进度条叠加在图片最底部 2px，几乎不遮挡内容
