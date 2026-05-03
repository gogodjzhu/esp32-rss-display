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

/* 显示无网络提示（黄色背景 + 居中感叹号图形） */
void ui_animation_show_no_network(void);

/* 仅更新底部进度条（0-100），不清空图片区域 */
void ui_animation_update_bottom_bar(int percent);

/* 将一行 RGB565 数据直接写入 TFT（绕过 LVGL，用于图片区域）
 * x: 列起始坐标，y: 行坐标，width: 像素数 */
void ui_animation_write_row(int x, int y, const uint16_t *rgb565, int width);

/* 切换到图片显示模式：清空 LVGL 屏幕（删除 spinner 等组件），
 * 预创建底部进度条，后续 lv_timer_handler 只重绘底部 20px */
void ui_animation_prepare_image_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_ANIMATION_H */
