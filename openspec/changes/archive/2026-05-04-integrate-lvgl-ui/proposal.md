# Proposal: integrate-lvgl-ui

## What

将 LVGL v9 UI 框架集成到主项目，新增 `lib/ui_animation/` 库，实现 ESP32-C3 + ST7789 TFT 显示屏的 UI 渲染能力，包括：

- Loading 动画（Spinner）
- 进度条（用于 RSS 加载进度等）
- 错误状态（红色背景）
- 连接成功 logo（绿色圆形 + 对勾线条）

## Why

当前主项目没有 TFT 显示屏驱动和 UI 能力。项目的核心用途是 RSS 新闻展示，图文内容由后端渲染成 JPEG 推给设备，设备端只需纯图形状态指示，无需文字渲染。

通过验证项目（`/tmp/opencode/lvgl_verify/`）已完整验证了所有关键参数：
- ST7789 SPI 驱动正确（MADCTL=0x60, 无 INVON, row_offset=0）
- LVGL v9.5.0 在 ESP-IDF 6.0.0 + ESP32-C3 下编译并运行正常
- 颜色格式 `LV_COLOR_FORMAT_RGB565_SWAPPED` 解决字节序问题

## Non-goals

- 不在设备端渲染文字（由后端 JPEG 承担）
- 不实现完整的 RSS 内容渲染（那是后续 change 的工作）
- 不修改现有 WiFi 配置门户逻辑
- 不引入触摸屏支持

## Constraints

- ESP32-C3 单核 160MHz，SRAM 235KB，Flash 4MB（factory 分区 2MB）
- `lib/` 下代码为纯 C，注释中文
