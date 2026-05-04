# Proposal: gpio-button-refresh

## What

利用 ESP32-C3 片上 GPIO9（BOOT 按钮）实现单次按压即刻刷新 RSS 条目的功能。

用户按下按钮后，当前正在倒计时的图片展示被中断，固件立即拉取并显示下一条 RSS 图片。

## Why

当前固件每隔 180 秒自动切换一次图片，用户没有主动控制能力。GPIO9 是板载唯一可用按钮，已有硬件支持，无需额外接线。增加按钮刷新可以显著提升交互体验，让用户随时获取最新新闻内容。

## Non-goals

- 不实现长按、双击等复杂手势
- 不修改自动轮换间隔逻辑
- 不添加防抖以外的额外按钮功能
- 不修改 WiFi 配网流程中的按钮行为

## Constraints

- GPIO9 为低电平有效（内部上拉），按下时拉低
- ESP32-C3 单核，需避免在中断中直接调用 FreeRTOS API（使用 TaskNotify 或队列通知任务）
- 不引入新的 FreeRTOS 任务，复用现有 `image_display_task`
