/**
 * @file image_fetcher.c
 * @brief 图片拉取实现 - HTTP 下载 JPEG + TJpgDec 解码 + TFT 写屏
 *
 * 解码流程：
 *   1. esp_http_client 下载完整 JPEG 到动态分配的 buffer
 *   2. TJpgDec（ROM 内置）解析 JPEG header，逐块解码 RGB888
 *   3. output callback 将 RGB888 转换为 RGB565，调用 ui_animation_write_row 写屏
 *   4. 释放 buffer
 *
 * 显示区域：y ∈ [0, 219]，共 220 行；底部 20px 留给进度条。
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp32c3/rom/tjpgd.h>
#include "sdkconfig.h"
#include "image_fetcher.h"
#include "ui_animation.h"

static const char *TAG = "IMG_FETCHER";

/* 配置常量 */
#define BACKEND_URL      CONFIG_BACKEND_URL
#define DEVICE_ID        CONFIG_DEVICE_ID
#define JPEG_BUF_MAX     (32 * 1024)   /* JPEG buffer 硬性上限 32KB（实际图片约 6~10KB） */
#define JPEG_POOL_SIZE   (5 * 1024)    /* TJpgDec 工作内存池 5KB（jd_prepare 最小需求约 3.5KB） */
#define IMAGE_DISP_ROWS  240           /* 图片写满全屏，底部 2px 进度条叠加 */
#define TFT_WIDTH        320

/* ---------- JSON 响应接收缓冲区 ---------- */
#define JSON_BUF_SIZE    512

/* 当前展示条目 ID（由 image_fetcher_get_next_url 更新，0 表示未获取） */
static uint32_t s_current_item_id = 0;

typedef struct {
    char   buf[JSON_BUF_SIZE];
    int    len;
} json_recv_ctx_t;

/* ---------- JPEG 解码上下文 ---------- */

typedef struct {
    uint8_t *data;     /* JPEG 数据 buffer */
    size_t   size;     /* buffer 总大小 */
    size_t   pos;      /* 当前读取位置 */
} jpeg_src_t;

/* TJpgDec 输入函数：从 jpeg_src_t 中读取数据 */
static UINT jpeg_infunc(JDEC *jdec, BYTE *buf, UINT nbytes)
{
    jpeg_src_t *src = (jpeg_src_t *)jdec->device;
    size_t avail = src->size - src->pos;
    if (nbytes > (UINT)avail) nbytes = (UINT)avail;
    if (buf && nbytes > 0) {
        memcpy(buf, src->data + src->pos, nbytes);
    }
    src->pos += nbytes;
    return nbytes;
}

/* TJpgDec 输出函数：RGB888 → RGB565，调用 ui_animation_write_row 写屏 */
static UINT jpeg_outfunc(JDEC *jdec, void *bitmap, JRECT *rect)
{
    (void)jdec;

    uint8_t  *src = (uint8_t *)bitmap;
    uint16_t  row_buf[TFT_WIDTH];
    int       block_w = rect->right  - rect->left + 1;
    int       block_h = rect->bottom - rect->top  + 1;

    for (int row = 0; row < block_h; row++) {
        int y = rect->top + row;

        /* 仅写入显示区域内的行 */
        if (y >= IMAGE_DISP_ROWS) {
            src += block_w * 3;
            continue;
        }

        /* RGB888 → RGB565（大端，适配 SPI 字节序） */
        for (int col = 0; col < block_w; col++) {
            uint8_t r = src[col * 3 + 0];
            uint8_t g = src[col * 3 + 1];
            uint8_t b = src[col * 3 + 2];
            uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            /* 字节序交换（与 LVGL LV_COLOR_FORMAT_RGB565_SWAPPED 一致） */
            row_buf[col] = (px >> 8) | (px << 8);
        }

        ui_animation_write_row(rect->left, y, row_buf, block_w);
        src += block_w * 3;
    }

    return 1;  /* 继续解码 */
}

/* ---------- HTTP 事件回调：接收 JSON 响应体 ---------- */

static esp_err_t json_http_event_cb(esp_http_client_event_t *evt)
{
    json_recv_ctx_t *ctx = (json_recv_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int remaining = JSON_BUF_SIZE - ctx->len - 1;
        int copy_len  = evt->data_len < remaining ? evt->data_len : remaining;
        if (copy_len > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, copy_len);
            ctx->len += copy_len;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ---------- 公开 API ---------- */

esp_err_t image_fetcher_init(void)
{
    ESP_LOGI(TAG, "图片拉取模块初始化 (backend: %s, device: %s)", BACKEND_URL, DEVICE_ID);
    return ESP_OK;
}

esp_err_t image_fetcher_get_next_url(char *url_buf, size_t buf_len)
{
    /* 构造请求 URL */
    char api_url[256];
    snprintf(api_url, sizeof(api_url), "%s/v1/device/%s/next", BACKEND_URL, DEVICE_ID);
    ESP_LOGI(TAG, "GET %s", api_url);

    json_recv_ctx_t ctx = {0};

    esp_http_client_config_t config = {
        .url          = api_url,
        .event_handler = json_http_event_cb,
        .user_data    = &ctx,
        .timeout_ms   = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 初始化失败");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 请求失败: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP 状态码: %d", status);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "响应 JSON: %s", ctx.buf);

    /* 从 JSON 响应中提取 image_url 字段值（简单字符串搜索，无需第三方库）
     * 期望格式：{ "image_url": "http://..." , ... }
     */
    const char *key = "\"image_url\"";
    char *key_pos = strstr(ctx.buf, key);
    if (!key_pos) {
        ESP_LOGE(TAG, "JSON 中无 image_url 字段");
        return ESP_FAIL;
    }

    /* 跳过 key、冒号、空白、引号 */
    char *val_start = key_pos + strlen(key);
    while (*val_start == ' ' || *val_start == ':' || *val_start == '\t') val_start++;
    if (*val_start != '"') {
        ESP_LOGE(TAG, "image_url 值格式错误");
        return ESP_FAIL;
    }
    val_start++;  /* 跳过开头引号 */

    /* 找到结尾引号 */
    char *val_end = strchr(val_start, '"');
    if (!val_end) {
        ESP_LOGE(TAG, "image_url 字符串未闭合");
        return ESP_FAIL;
    }

    size_t val_len = (size_t)(val_end - val_start);
    if (val_len == 0 || val_len >= buf_len) {
        ESP_LOGE(TAG, "image_url 长度异常: %d", (int)val_len);
        return ESP_FAIL;
    }

    memcpy(url_buf, val_start, val_len);
    url_buf[val_len] = '\0';
    ESP_LOGI(TAG, "image_url: %s", url_buf);

    /* 解析 item_id 字段（整数），缓存供评分提交使用 */
    const char *id_key = "\"item_id\"";
    char *id_pos = strstr(ctx.buf, id_key);
    if (id_pos) {
        char *colon = strchr(id_pos + strlen(id_key), ':');
        if (colon) {
            s_current_item_id = (uint32_t)strtoul(colon + 1, NULL, 10);
            ESP_LOGI(TAG, "item_id: %" PRIu32, s_current_item_id);
        } else {
            s_current_item_id = 0;
        }
    } else {
        ESP_LOGW(TAG, "JSON 中无 item_id 字段，评分提交将被跳过");
        s_current_item_id = 0;
    }

    return ESP_OK;
}

esp_err_t image_fetcher_download_and_show(const char *url)
{
    ESP_LOGI(TAG, "下载图片: %s", url);

    /* 先 open 连接，获取 Content-Length，再按实际大小分配 buffer */
    esp_http_client_config_t config = {
        .url         = url,
        .timeout_ms  = 15000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 初始化失败");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open 失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP 状态码: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 确定分配大小：Content-Length 未知时用上限，已知时按实际大小（不超上限） */
    size_t alloc_size = JPEG_BUF_MAX;
    if (content_len > 0 && content_len < (int)JPEG_BUF_MAX) {
        alloc_size = (size_t)content_len;
    } else if (content_len >= (int)JPEG_BUF_MAX) {
        ESP_LOGE(TAG, "Content-Length %d 超出上限 %d", content_len, (int)JPEG_BUF_MAX);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *jpeg_buf = (uint8_t *)malloc(alloc_size);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "malloc JPEG buffer 失败（需要 %d bytes，可用最大块 %d bytes）",
                 (int)alloc_size,
                 (int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 读取响应体 */
    int total_read = 0;
    while (total_read < (int)alloc_size) {
        int n = esp_http_client_read(client, (char *)jpeg_buf + total_read,
                                     (int)alloc_size - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGE(TAG, "未读取到任何数据");
        free(jpeg_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "JPEG 下载完成，大小: %d bytes（分配: %d bytes）", total_read, (int)alloc_size);

    /* 分配 TJpgDec 工作内存池 */
    void *pool = malloc(JPEG_POOL_SIZE);
    if (!pool) {
        ESP_LOGE(TAG, "malloc TJpgDec 内存池失败");
        free(jpeg_buf);
        return ESP_FAIL;
    }

    /* 初始化 TJpgDec 解码器 */
    JDEC   jdec;
    jpeg_src_t src = {
        .data = jpeg_buf,
        .size = (size_t)total_read,
        .pos  = 0,
    };

    JRESULT jres = jd_prepare(&jdec, jpeg_infunc, pool, JPEG_POOL_SIZE, &src);
    if (jres != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare 失败: %d", jres);
        free(pool);
        free(jpeg_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "JPEG 尺寸: %dx%d，开始解码...", jdec.width, jdec.height);

    /* 解码（scale=0 即原始尺寸），outfunc 负责写屏 */
    jres = jd_decomp(&jdec, jpeg_outfunc, 0);
    if (jres != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp 返回: %d（可能为正常中断）", jres);
    }

    free(pool);
    free(jpeg_buf);
    ESP_LOGI(TAG, "图片显示完成");
    return ESP_OK;
}

/* ---------- 评分提交 ---------- */

esp_err_t image_fetcher_submit_rating(int rating)
{
    /* item_id 或 rating 无效时直接失败，不发请求 */
    if (s_current_item_id == 0) {
        ESP_LOGW(TAG, "item_id 未获取，跳过评分提交");
        return ESP_FAIL;
    }
    if (rating <= 0) {
        ESP_LOGW(TAG, "rating=%d 无效，跳过评分提交", rating);
        return ESP_FAIL;
    }

    /* 构造请求 URL */
    char api_url[256];
    snprintf(api_url, sizeof(api_url),
             "%s/v1/item/%" PRIu32 "/rating", BACKEND_URL, s_current_item_id);

    /* 构造 JSON body */
    char body[128];
    snprintf(body, sizeof(body),
             "{\"rating\":%d,\"device_id\":\"%s\"}", rating, DEVICE_ID);

    ESP_LOGI(TAG, "POST %s body=%s", api_url, body);

    esp_http_client_config_t config = {
        .url        = api_url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 初始化失败");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "评分 HTTP 请求失败: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "评分 HTTP 状态码: %d", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "评分提交成功，item_id=%" PRIu32 " rating=%d", s_current_item_id, rating);
    return ESP_OK;
}
