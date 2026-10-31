#ifndef CONTENT_ITEM_H
#define CONTENT_ITEM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui_limits.h"
#ifdef __cplusplus
extern "C" {
#endif
#define CONTENT_TITLE_MAX 384
#define CONTENT_TIME_MAX 32
#define CONTENT_URL_MAX 512
#define CONTENT_STOCK_MAX UI_STOCK_MAX
#define CONTENT_STOCK_NAME_MAX 32

/* 行情涨跌以 0.01% 为单位，正数上涨、负数下跌，避免浮点比较误差 */
typedef struct {
    char name[CONTENT_STOCK_NAME_MAX];   /* 原始名称，按码点安全截断，不做显示裁剪 */
    int32_t change_bp;
} content_stock_t;

typedef struct {
    uint32_t id;
    char title[CONTENT_TITLE_MAX];
    char published_at[CONTENT_TIME_MAX];
    char image_url[CONTENT_URL_MAX];
    /* 新鲜度：由后端在 next 响应中计算好下发（设备无时钟）。
     * freshness_known 为假（旧后端 / 字段缺失）时首行按绝对时间显示、不画圆点。 */
    bool    freshness_known;
    int64_t age_sec;    /* 已发布秒数，>= 0；缺失按 0（最新） */
    uint8_t open_count; /* NFC 打开次数，缺失按 0 */
    content_stock_t stocks[CONTENT_STOCK_MAX];
    uint8_t stock_count;
} content_item_t;

/* 解析失败不改变调用方已有内容。
 * stocks 为可选装饰字段：非法条目逐条丢弃、超过 CONTENT_STOCK_MAX 项截断，
 * 不影响内容本身。 */
bool content_item_parse(const char *json, content_item_t *out);
#ifdef __cplusplus
}
#endif
#endif
