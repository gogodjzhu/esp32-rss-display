# Proposal: backend-image-display

## What

从后端 API 定时拉取图片并显示到 TFT 屏幕，替换当前 benchmark demo 的主循环逻辑：

- 每 30 秒调用一次后端 `/v1/device/{id}/next`，获取 `image_url`
- 下载该 URL 的 320×240 JPEG 图片
- 解码 JPEG 并通过 SPI 直接写入 TFT 显示屏（前 220 行）
- 底部 20px 显示 LVGL 进度条，倒计时 30 秒后刷新
- 若 WiFi 未连接，屏幕显示"无网络"提示

## Why

当前固件仅运行 LVGL benchmark demo，没有实际业务逻辑。后端已就绪，设备端需要接入数据流并将服务端渲染好的图片内容呈现到屏幕上，实现系统端到端打通。

## Non-goals

- 不在设备端渲染文字或 RSS 内容（由后端 JPEG 承担）
- 不实现图片缓存（每次实时下载）
- 不修改 WiFi 配网逻辑
- 不处理多设备 ID 切换

## Constraints

- ESP32-C3 单核 160MHz，SRAM 235KB，Flash 4MB
- `lib/` 下代码为纯 C，注释中文
- 使用 ESP-IDF 内置 `esp_http_client` 和 `cJSON`
- JPEG 解码使用 ESP-IDF 内置 `esp_jpeg`（TJpgDec）
- 图片全屏 320×240，设备 ID 通过 Kconfig 配置
