/**
 * @file ui_animation.h
 * @brief 显示屏 UI 动画库公开 API
 *
 * 封装 ST7789 SPI 驱动和 LVGL v9 初始化，提供简单的 UI 状态接口。
 * 使用方式：
 *   1. app_main() 中调用 ui_animation_init() 一次
 *   2. 根据状态调用 show_loading / show_progress / show_error
 *   3. 主循环中定期调用 ui_animation_task() 驱动渲染
 */

#ifndef UI_ANIMATION_H
#define UI_ANIMATION_H

#ifdef __cplusplus
extern "C" {
#endif

void ui_animation_init(void);
void ui_animation_show_loading(void);
void ui_animation_show_progress(int percent);
void ui_animation_show_error(void);
void ui_animation_show_ok(void);
void ui_animation_task(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_ANIMATION_H */
