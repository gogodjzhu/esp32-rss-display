/**
 * @file image_fetcher.h
 * @brief 图片拉取模块 - 从后端获取图片 URL 并下载显示到 TFT
 */

#ifndef IMAGE_FETCHER_H
#define IMAGE_FETCHER_H

#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化图片拉取模块（预留，当前无状态）
 * @return ESP_OK 成功
 */
esp_err_t image_fetcher_init(void);

/**
 * @brief 从后端获取下一条内容的图片 URL
 *
 * 直接使用 backend_url 作为完整请求地址（不再自动拼接尾缀），
 * 解析响应 JSON 中的 image_url 字段，写入 url_buf。
 *
 * @param url_buf 存储图片 URL 的缓冲区
 * @param buf_len 缓冲区长度
 * @return ESP_OK 成功，ESP_FAIL 请求或解析失败
 */
esp_err_t image_fetcher_get_next_url(char *url_buf, size_t buf_len);

/**
 * @brief 下载指定 URL 的 JPEG 图片并显示到 TFT 屏幕
 *
 * 下载完成后解码 JPEG（RGB888 → RGB565），通过 ui_animation_write_row
 * 逐行写入 TFT 前 220 行（底部 20px 留给进度条）。
 *
 * @param url JPEG 图片的完整 URL
 * @return ESP_OK 成功，ESP_FAIL 下载或解码失败
 */
esp_err_t image_fetcher_download_and_show(const char *url);

/**
 * @brief 向后端提交当前条目的评分
 *
 * 使用 image_fetcher_get_next_url 缓存的 item_id 发起请求。
 * item_id 为 0 时直接返回 ESP_FAIL（不发请求，防止污染数据）。
 *
 * @param rating 评分值 1-5（0 时返回 ESP_FAIL）
 * @return ESP_OK 提交成功（HTTP 2xx），ESP_FAIL 其他情况
 */
esp_err_t image_fetcher_submit_rating(int rating);

/**
 * @brief 获取当前实际使用的 backend URL
 *
 * 返回 image_fetcher_init() 加载后的 URL（NVS 值或硬编码 fallback）。
 * 必须在 image_fetcher_init() 之后调用。
 *
 * @return 指向内部静态字符串的指针，调用方不可修改或释放
 */
const char *image_fetcher_get_backend_url(void);

/**
 * @brief 获取当前加载的自定义 HTTP headers（JSON 数组字符串）
 *
 * 返回 image_fetcher_init() 从 NVS 加载的 headers JSON，格式为：
 * [{"k":"Header-Name","v":"Header-Value"},...]。
 * 无自定义 headers 时返回 "[]"。
 * 必须在 image_fetcher_init() 之后调用。
 *
 * @return 指向内部静态字符串的指针，调用方不可修改或释放
 */
const char *image_fetcher_get_http_headers(void);

/**
 * @brief 获取最近一次 image_fetcher_get_next_url() 解析出的条目标题（UTF-8）
 *
 * 用于在图片底部浮层显示标题，可能包含中文。无标题时返回空串。
 *
 * @return 指向内部静态字符串的指针，调用方不可修改或释放
 */
const char *image_fetcher_get_title(void);

/**
 * @brief 获取最近一次 image_fetcher_get_next_url() 解析出的发布时间字符串
 *
 * 格式为后端下发的本地时间展示串（YYYY-MM-DD HH:MM），无发布时间时返回空串。
 * 用于在标题浮层首行显示内容时间。
 *
 * @return 指向内部静态字符串的指针，调用方不可修改或释放
 */
const char *image_fetcher_get_published_at(void);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_FETCHER_H */
