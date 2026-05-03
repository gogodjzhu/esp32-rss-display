# Design: integrate-lvgl-ui

## 架构概览

新增一个库到主项目 `lib/` 目录：

```
lib/
  ui_animation/     # UI 动画库
    ui_animation.c     # ST7789 驱动 + LVGL 初始化 + UI 组件
    ui_animation.h     # 公开 API
    library.json       # PlatformIO 库配置
```

## ST7789 驱动参数（已验证）

| 参数 | 值 | 说明 |
|------|-----|------|
| MADCTL | `0x60` | MX\|MV 横屏，无 BGR bit |
| INVON | 无 | 不发送 `0x21`，避免颜色反转 |
| row_offset | `0` | 无行偏移 |
| col_offset | `0` | 无列偏移 |
| 分辨率 | 320×240 | 横屏 |

## LVGL 配置（lv_conf.h）

- `LV_COLOR_DEPTH 16`
- `LV_COLOR_FORMAT_RGB565_SWAPPED` — 解决 SPI MSB/LSB 字节序
- 不启用任何字体（UI 全部使用纯图形组件）
- 启用 widget：`LV_USE_SPINNER`, `LV_USE_BAR`, `LV_USE_ARC`, `LV_USE_LINE`, `LV_USE_LABEL`（后者为 lv_image 内部依赖）
- 分块渲染 buffer：`320×20行 × 2`（双 buffer，各 25.6KB）

## SPI 配置

- MOSI=6, SCLK=5, CS=7, DC=3, RST=10
- 频率：40MHz
- SPI 分段传输：每次 ≤ 4092 bytes（ESP-IDF DMA 限制）

## LVGL tick 方式

渲染循环内用 `esp_timer_get_time()` delta 驱动 `lv_tick_inc()`，不额外创建 timer task。

## ui_animation 公开 API

```c
/* 初始化显示屏和 LVGL（在 app_main 中调用一次） */
void ui_animation_init(void);

/* 显示 loading 界面（居中 spinner） */
void ui_animation_show_loading(void);

/* 显示进度条（0-100） */
void ui_animation_show_progress(int percent);

/* 显示错误状态（红色背景） */
void ui_animation_show_error(void);

/* 显示连接成功 logo（绿色圆形 + 白色对勾线条） */
void ui_animation_show_ok(void);

/* 驱动 LVGL 渲染循环（在主循环中定期调用） */
void ui_animation_task(void);
```

## 固件大小

| 组件 | 大小 |
|------|------|
| LVGL + UI 动画库 | ~650KB |
| 主项目代码（RSS/WiFi/HTTP） | ~100KB |
| 总计 | ~1.38MB（factory 2MB 以内） |
