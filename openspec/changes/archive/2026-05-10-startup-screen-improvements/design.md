# Design: startup-screen-improvements

## Startup Flow (Before vs After)

```
BEFORE                              AFTER
──────────────────────────────────────────────────────────────────
app_main()                          app_main()
  show_loading()  [spinner]           show_loading()  [spinner]
  wifi_manager_smart_connect()        wifi_manager_smart_connect()
  │                                   │
  ├─ no credentials                   ├─ no credentials
  │    show_no_network()                   show_no_network("ESP32-Config")
  │    [yellow ! icon only]               [! icon + text:
  │                                        "Connect to WiFi hotspot"
  │                                        "SSID: ESP32-Config"
  │                                        "Open: 192.168.4.1"]
  │
  └─ connected                        └─ connected
       → image_display_task                show_connecting(backend_url)
         (no transition screen)            [spinner + "Connecting to"
                                            "http://..."]
                                           → image_display_task
```

---

## image_fetcher Module Changes

### Remove Kconfig dependency

```c
/* 删除: #define BACKEND_URL CONFIG_BACKEND_URL */
/* 新增: 硬编码 fallback，不再出现在 menuconfig */
#define DEFAULT_BACKEND_URL "http://192.168.3.112:8080"

static char s_backend_url[128];  /* 已存在，无变化 */
```

`image_fetcher_init()` 逻辑不变（NVS 优先，fallback 到 `DEFAULT_BACKEND_URL`），只是把宏名从 `CONFIG_BACKEND_URL` 改为 `DEFAULT_BACKEND_URL`，并删除 `#include "sdkconfig.h"`（若仅用于该宏）。

### New getter function

```c
/* image_fetcher.h */
const char *image_fetcher_get_backend_url(void);

/* image_fetcher.c */
const char *image_fetcher_get_backend_url(void)
{
    return s_backend_url;
}
```

`main.cpp` 在 `image_fetcher_init()` 调用后调用此 getter 获取实际 URL 用于显示。

---

## ui_animation Module Changes

### `show_no_network` signature change

```c
/* 旧: */ void ui_animation_show_no_network(void);
/* 新: */ void ui_animation_show_no_network(const char *ap_ssid);
```

在现有黄色感叹号图标下方添加三行 LVGL label：

```
┌──────────────────────────────────┐
│                                  │
│          ⚠  (yellow circle)      │
│                                  │
│   Connect to WiFi hotspot        │  ← label 1, white, center
│   SSID: ESP32-Config             │  ← label 2, yellow, center
│   Open: 192.168.4.1              │  ← label 3, white/dim, center
│                                  │
└──────────────────────────────────┘
```

实现要点：
- `ap_ssid` 插入 label 2 的格式字符串中（`lv_label_set_text_fmt`）
- IP 固定为 `"192.168.4.1"`（ESP-IDF AP 模式默认网关）
- font 使用 `LV_FONT_DEFAULT`（Montserrat 14px，无需额外引入）
- label 依次 `LV_ALIGN_CENTER` + y offset 排列，位于圆圈下方

### New `show_connecting` function

```c
/* ui_animation.h */
void ui_animation_show_connecting(const char *backend_url);
```

```
┌──────────────────────────────────┐
│                                  │
│          ⟳  (spinner, 60x60)    │
│                                  │
│   Connecting to server...        │  ← label 1, white, center
│   http://192.168.3.112:8080      │  ← label 2, dim, center, small
│                                  │
└──────────────────────────────────┘
```

实现要点：
- 复用 `ui_animation_show_loading()` 的黑色背景 + spinner
- 追加两个 label（`show_loading` 调用完后继续添加子组件）
- URL label 字体同 `LV_FONT_DEFAULT`，颜色设为灰色 `0x888888`
- 调用后需 `ui_animation_task()` 驱动一次渲染，否则画面不更新

---

## main.cpp Changes

### AP mode

```c
/* 旧: */
ui_animation_show_no_network();

/* 新: */
wifi_info_t *info = wifi_manager_get_info();
ui_animation_show_no_network(info->ap_ssid);
```

### STA connected — connecting screen

```c
/* 在 button_init() 和 http_server_start() 之前插入: */
image_fetcher_init();
ui_animation_show_connecting(image_fetcher_get_backend_url());
/* 驱动 LVGL 渲染一帧，确保画面更新 */
ui_animation_task();
vTaskDelay(pdMS_TO_TICKS(100));
```

然后 `image_display_task` 内部**不再重复调用** `image_fetcher_init()`（因为已在 `app_main` 中提前调用）。

> **注意**：`image_fetcher_init()` 前移到 `app_main`，`image_display_task` 开头的 `image_fetcher_init()` 调用需删除，避免二次初始化覆盖已加载的 NVS 值（虽然结果相同，但语义上 init 只应调用一次）。

---

## Kconfig Change

删除 `src/Kconfig` 中整个 `config BACKEND_URL` 块：

```
config BACKEND_URL        ← 删除此行
    string "Backend server URL"   ← 删除
    default "http://..."          ← 删除
    help                          ← 删除
        后端服务器地址...          ← 删除
```

`sdkconfig.defaults` 和 `sdkconfig.airm2m_core_esp32c3` 中若有 `CONFIG_BACKEND_URL=...` 行也一并删除（避免编译警告）。

---

## Key Parameters

| Item | Value |
|------|-------|
| `DEFAULT_BACKEND_URL` | `"http://192.168.3.112:8080"` |
| AP gateway IP | `"192.168.4.1"` (ESP-IDF default) |
| Font | `LV_FONT_DEFAULT` (Montserrat 14px) |
| Connecting screen display time | ~100ms (just one render frame, then task takes over) |
