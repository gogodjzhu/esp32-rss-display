#ifndef BANNER_H
#define BANNER_H
#include <stdint.h>
/* 字形和背景缓存归 UI 任务所有，禁止跨任务调用 */
void banner_prepare(const char *title);
void banner_release(void);
void banner_apply_row(uint16_t *pixels, int y, int x, int width);
void banner_begin_animation(void);
void banner_tick(void);
void banner_show_solid(uint32_t color, const char *title);
#endif
