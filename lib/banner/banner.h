#ifndef BANNER_H
#define BANNER_H
#include <stdint.h>
#include "ui_limits.h"

#define BANNER_STOCK_MAX UI_STOCK_MAX

/* 一行行情中的单个标的；change_bp 以 0.01% 为单位，正数上涨、负数下跌 */
typedef struct {
    const char *name;
    int32_t     change_bp;
} banner_stock_t;

typedef struct {
    const char         *time;        /* 右侧首行时间，可为空 */
    const char         *title;       /* 右侧标题，可多行自动换行 */
    const banner_stock_t *stocks;    /* 左侧行情，逐条上下滚动 */
    int                 stock_count; /* 0 表示不显示行情栏 */
} banner_content_t;

/* 字形和背景缓存归 UI 任务所有，禁止跨任务调用。
 * content 及其内部指针只在 banner_prepare() 调用期间读取，不保留。 */
void banner_prepare(const banner_content_t *content);
void banner_set_progress(int percent);
void banner_release(void);
void banner_apply_row(uint16_t *pixels, int y, int x, int width);
void banner_begin_animation(void);
void banner_tick(void);
void banner_show_solid(uint32_t color, const banner_content_t *content);
#endif
