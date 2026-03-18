/**
 * @file rss_reader.h
 * @brief RSS Reader - 定期获取RSS feed并随机缓存一条
 */

#ifndef RSS_READER_H
#define RSS_READER_H

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSS_ITEM_TITLE_MAX  256
#define RSS_ITEM_LINK_MAX   512
#define RSS_ITEM_DATE_MAX   64

/**
 * @brief RSS缓存条目结构
 */
typedef struct {
    char title[RSS_ITEM_TITLE_MAX];
    char link[RSS_ITEM_LINK_MAX];
    char pubDate[RSS_ITEM_DATE_MAX];
    bool valid;
} rss_cached_item_t;

/**
 * @brief 初始化RSS Reader
 * @return ESP_OK 成功
 */
esp_err_t rss_reader_init(void);

/**
 * @brief RSS Reader主任务 - 定时获取RSS并随机选取条目
 * @param pvParameters FreeRTOS任务参数
 */
void rss_reader_task(void *pvParameters);

/**
 * @brief 获取当前缓存的RSS条目
 * @return 指向rss_cached_item_t的指针
 */
rss_cached_item_t* rss_reader_get_cached_item(void);

#ifdef __cplusplus
}
#endif

#endif // RSS_READER_H
