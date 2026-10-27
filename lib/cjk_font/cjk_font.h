/**
 * @file cjk_font.h
 * @brief 中文点阵字体（Tiny TTF）加载模块
 *
 * 从专用的 font flash 分区映射中文子集 TTF（assets/fonts/cjk_subset.ttf，
 * 由 tools/gen_cjk_font.py 从开源字体裁剪而来），交给 LVGL 内置的 Tiny TTF
 * 按需光栅化。字体数据常驻 flash（mmap，零拷贝），不同字号各自维护字形缓存。
 */

#ifndef CJK_FONT_H
#define CJK_FONT_H

#include <stdbool.h>
#include <esp_err.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化中文字体模块
 *
 * 定位 label 为 "font" 的 data 分区并 mmap，随后创建默认字号字体。
 * 失败时不致命：cjk_font_get() 会返回 NULL，调用方可回退。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 无 font 分区；其他 mmap 错误
 */
esp_err_t cjk_font_init(void);

/** @brief 中文字体是否可用 */
bool cjk_font_available(void);

/**
 * @brief 获取指定像素字号的中文字体（内部缓存，重复调用返回同一对象）
 * @param size_px 字号（像素）；<=0 时使用默认字号
 * @return 字体对象；模块未初始化或创建失败时返回 NULL
 */
const lv_font_t *cjk_font_get(int size_px);

#ifdef __cplusplus
}
#endif

#endif /* CJK_FONT_H */
