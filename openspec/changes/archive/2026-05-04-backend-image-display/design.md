# Design: backend-image-display

## 架构概览

```
app_main()
    │
    ├─ ui_animation_init()          ← 已有：ST7789 + LVGL 初始化
    ├─ wifi_manager_init()          ← 已有
    ├─ wifi_manager_smart_connect() ← 已有
    │       │
    │    失败└─ ui_animation_show_no_network() → 死循环
    │
    └─ xTaskCreate(image_display_task, 8192)
               │
               └─ loop (30s 周期):
                   ├─ image_fetcher_get_next_url(url_buf)
                   ├─ image_fetcher_download_and_show(url_buf)
                   └─ for t in 0..30:
                         ui_animation_show_bottom_bar(t * 100 / 30)
                         vTaskDelay(1s)
```

## 新增模块：lib/image_fetcher/

```
lib/image_fetcher/
    image_fetcher.h   ← 公开 API
    image_fetcher.c   ← HTTP + JSON + JPEG 解码 + SPI 写屏
```

### 公开 API

```c
/* 初始化（传入 SPI 写屏回调） */
esp_err_t image_fetcher_init(void);

/* GET /v1/device/{id}/next，解析 image_url 字段写入 url_buf */
esp_err_t image_fetcher_get_next_url(char *url_buf, size_t buf_len);

/* 下载 url 处的 JPEG 并直接写入 TFT 前 220 行 */
esp_err_t image_fetcher_download_and_show(const char *url);
```

### 关键实现细节

**JSON 解析**

使用 ESP-IDF 内置 cJSON：
```
GET /v1/device/{id}/next
→ { "image_url": "http://...", ... }
→ cJSON_GetObjectItem(root, "image_url")->valuestring
```
HTTP 响应先完整接收（响应体约 200 bytes），再 cJSON 解析。

**JPEG 下载**

- 使用 `esp_http_client_perform()` 流式接收
- 动态 malloc 接收 buffer（上限 80KB，下载前检查 Content-Length）
- 若 Content-Length 超限则报错
- 下载完成后再解码，释放 buffer 前完成 SPI 写入

**JPEG 解码 + TFT 写入**

使用 ESP-IDF `esp_jpeg` 组件（TJpgDec 封装）：
- 解码输出 callback 接收每行 RGB565 数据
- callback 内调用 `ui_animation_write_row(y, rgb565_buf, width)`
- 图片写入行范围：y ∈ [0, 239]（全屏 240 行，进度条 2px 叠加在最底部）

```
┌────────────────────────────┐ y=0
│                            │
│      JPEG 图片区域         │
│      320 × 240（全屏）     │
│                            │
│  ████████████░░░░░░░░░░░░  │ y=238 进度条（2px，LVGL bar 叠加）
└────────────────────────────┘ y=239
```

## ui_animation 新增接口

```c
/* 显示无网络提示（黄色背景 + 居中 X 图形） */
void ui_animation_show_no_network(void);

/* 仅更新底部进度条（不清空屏幕，保留图片区域） */
void ui_animation_update_bottom_bar(int percent);

/* 将一行 RGB565 数据写入 TFT（image_fetcher 解码回调使用） */
void ui_animation_write_row(int x, int y, const uint16_t *rgb565, int width);
```

`ui_animation_write_row` 直接调用内部 `tft_set_window` + SPI 传输，绕过 LVGL（图片区域不走 LVGL 渲染）。

`ui_animation_update_bottom_bar` 使用 LVGL bar widget，area 限定在 y∈[238,239]（高 2px），调用 `lv_timer_handler()` 刷新。

## Kconfig 新增配置

在 `src/Kconfig` `menu "RSS Display Configuration"` 内追加：

```
config BACKEND_URL
    string "Backend server URL"
    default "http://192.168.3.106:8080"

config DEVICE_ID
    string "Device ID"
    default "test-device-001"
```

在 `sdkconfig.defaults` 无需额外设置（使用默认值即可）。

## 内存预算

| 用途 | 大小 |
|------|------|
| LVGL 双渲染 buffer（已有） | 25.6KB × 2 |
| JPEG 下载 buffer（动态 malloc）| 最大 80KB |
| TJpgDec 工作区 | ~3KB |
| image_display_task 栈 | 8KB |
| url / JSON 缓冲 | ~1KB |

JPEG buffer 用完立即 free，峰值约 110KB，在 235KB SRAM 范围内可行。

## FreeRTOS 任务设计

| 任务 | 栈 | 优先级 | 说明 |
|------|----|--------|------|
| app_main（内联） | 4KB | 1 | 初始化后创建任务即退出 |
| image_display_task | 8KB | 5 | 主业务循环 |
| LVGL（无独立任务） | — | — | 在 image_display_task 内驱动 |

LVGL `lv_timer_handler()` 在进度条更新循环内调用，不需要独立 task。
