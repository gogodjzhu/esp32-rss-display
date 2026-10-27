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

#include <stdbool.h>
#include <stdint.h>

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
void ui_animation_show_no_network(const char *ap_ssid);

/**
 * @brief 显示"正在连接服务器"过渡画面
 *
 * 显示 spinner + backend URL 文字，需在 image_fetcher_init() 之后调用。
 * 内部调用 ui_animation_task() 驱动一帧渲染。
 *
 * @param backend_url 实际使用的后端地址字符串
 */
void ui_animation_show_connecting(const char *backend_url);

/* 仅更新底部进度条（0-100），不清空图片区域 */
void ui_animation_update_bottom_bar(int percent);

/* 将一行 RGB565 数据直接写入 TFT（绕过 LVGL，用于图片区域）
 * x: 列起始坐标，y: 行坐标，width: 像素数 */
void ui_animation_write_row(int x, int y, const uint16_t *rgb565, int width);

/* 切换到图片显示模式：清空 LVGL 屏幕（删除 spinner 等组件），
 * 预创建底部进度条，后续 lv_timer_handler 只重绘底部 20px */
void ui_animation_prepare_image_mode(void);

/* 在底部 2px 区域绘制评价指示条（直接写 TFT，绕过 LVGL）
 * sel: 0=NONE（两段均暗）, 1=LIKE（右半亮绿）, 2=DISLIKE（左半亮红） */
void ui_animation_show_rating_bar(int sel);

/* 显示评分提交结果状态栏（覆盖底部 2px，直接写 TFT）
 * success=true：全绿 (0x2ECC71)；success=false：全红 (0xE74C3C) */
void ui_animation_show_submit_bar(bool success);

/* 中文点阵字体测试画面：显示多段常用汉字/标点/中英混排，用于验证字体 */
void ui_animation_show_font_test(void);

/* ---------- 图片标题浮层（BBC 风格上下滚动 banner + 中文标题） ---------- */

/* 预排版标题浮层：解析 UTF-8、换行、取字形位图。应在写图片前调用。
 * title 为 NULL/空串时不显示浮层。排版后同一时刻只显示一行，
 * 由 tick 驱动整行上下滚动切换到下一行。需配套调用 begin_animation。 */
void ui_animation_banner_prepare(const char *title);

/* 释放标题浮层占用的字形缓存与背景底图 */
void ui_animation_banner_release(void);

/* 对一行 RGB565（字节交换序，与 ui_animation_write_row 一致）叠加
 * 半透明 banner 背景与标题文字。screen_y 为该行屏幕 Y 坐标，
 * row 覆盖屏幕 x ∈ [x_start, x_start+width)。
 * 图片解码阶段会同时把 banner 窗口的背景像素缓存下来供滚动重绘。 */
void ui_animation_banner_apply_row(uint16_t *row, int screen_y, int x_start, int width);

/* 图片/纯色背景写屏完成后调用：结束背景捕获并启动滚动动画。
 * 此后需由主循环周期性调用 ui_animation_banner_tick() 推进动画。 */
void ui_animation_banner_begin_animation(void);

/* 推进标题浮层滚动动画（整行滚到下一行）。由主循环周期性调用。
 * 行数 <= 1 或浮层未激活时不产生任何写屏动作。 */
void ui_animation_banner_tick(void);

/* 无图片时的纯色背景直接显示（整屏填充 bg_rgb24）并在其上叠加标题浮层，
 * 完成后同样启动滚动动画（需配合 tick）。 */
void ui_animation_show_solid_banner(uint32_t bg_rgb24, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* UI_ANIMATION_H */
