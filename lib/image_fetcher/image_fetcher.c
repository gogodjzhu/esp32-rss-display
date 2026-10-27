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
/* TJpgDec：使用 ESP-ROM 内置的 JPEG 解码器，头路径由 esp_rom 按 IDF_TARGET 解析 */
#include <rom/tjpgd.h>
#include "sdkconfig.h"
#include "image_fetcher.h"
#include "ui_animation.h"
#include "nvs_manager.h"

static const char *TAG = "IMG_FETCHER";

/* 配置常量 */
#define DEVICE_ID           CONFIG_DEVICE_ID
/* 硬编码 fallback，不再通过 Kconfig 暴露给 menuconfig；
 * 实际运行时优先使用 NVS 中通过 Web 页面保存的值 */
#define DEFAULT_BACKEND_URL "http://192.168.3.122:8080"

/* 运行时 backend URL，image_fetcher_init() 时从 NVS 加载，fallback 到 Kconfig 默认值 */
static char s_backend_url[128];
/* 自定义 HTTP headers JSON 数组字符串，image_fetcher_init() 时从 NVS 加载 */
static char s_http_headers[512];
#define JPEG_BUF_MAX     (32 * 1024)   /* JPEG buffer 硬性上限 32KB（实际图片约 6~10KB） */
#define JPEG_POOL_SIZE   (5 * 1024)    /* TJpgDec 工作内存池 5KB（jd_prepare 最小需求约 3.5KB） */
#define IMAGE_DISP_ROWS  240           /* 图片写满全屏，底部 2px 进度条叠加 */
#define TFT_WIDTH        320

/* 无图片（或图片不可用）时的纯色背景，深蓝灰，衬托白色标题 */
#define SOLID_BG_RGB24   0x203040

/* ---------- JSON 响应接收缓冲区 ---------- */
#define JSON_BUF_SIZE    2048

/* 当前展示条目标题（UTF-8，由 image_fetcher_get_next_url 更新） */
#define TITLE_BUF_SIZE   384
static char s_current_title[TITLE_BUF_SIZE];

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

        /* 叠加底部标题浮层（半透明 banner + 中文） */
        ui_animation_banner_apply_row(row_buf, y, rect->left, block_w);

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
    /* 先以 Kconfig 默认值初始化，再尝试从 NVS 覆盖 */
    strncpy(s_backend_url, DEFAULT_BACKEND_URL, sizeof(s_backend_url) - 1);
    s_backend_url[sizeof(s_backend_url) - 1] = '\0';

    char nvs_url[128] = {0};
    size_t nvs_url_len = sizeof(nvs_url);
    if (nvs_manager_get_str("backend_url", nvs_url, &nvs_url_len) == ESP_OK && strlen(nvs_url) > 0) {
        strncpy(s_backend_url, nvs_url, sizeof(s_backend_url) - 1);
        ESP_LOGI(TAG, "从 NVS 加载 backend_url: %s", s_backend_url);
    } else {
        ESP_LOGI(TAG, "使用默认 backend_url: %s", s_backend_url);
    }

    ESP_LOGI(TAG, "图片拉取模块初始化 (backend: %s, device: %s)", s_backend_url, DEVICE_ID);

    /* 从 NVS 加载自定义 HTTP headers */
    char headers_buf[sizeof(s_http_headers)] = {0};
    size_t headers_len = sizeof(headers_buf);
    if (nvs_manager_get_str("http_headers", headers_buf, &headers_len) == ESP_OK && strlen(headers_buf) > 0) {
        strncpy(s_http_headers, headers_buf, sizeof(s_http_headers) - 1);
        s_http_headers[sizeof(s_http_headers) - 1] = '\0';
        ESP_LOGI(TAG, "从 NVS 加载 http_headers: %s", s_http_headers);
    } else {
        s_http_headers[0] = '\0';
        ESP_LOGI(TAG, "无自定义 HTTP headers");
    }

    return ESP_OK;
}

/* 解析 s_http_headers JSON 数组，为 HTTP 客户端设置自定义 headers */
static void apply_custom_headers(esp_http_client_handle_t client)
{
    if (s_http_headers[0] == '\0') return;

    const char *p = s_http_headers;
    while ((p = strstr(p, "{\"k\":\"")) != NULL) {
        p += 6;
        char key[128] = {0};
        int i = 0;
        while (*p && *p != '"' && i < 127) key[i++] = *p++;
        if (*p == '"') p++;

        p = strstr(p, "\"v\":\"");
        if (!p) break;
        p += 5;
        char val[512] = {0};
        i = 0;
        while (*p && *p != '"' && i < 511) {
            if (*p == '\\' && *(p + 1) != '\0') p++;
            val[i++] = *p++;
        }

        if (key[0]) {
            ESP_LOGI(TAG, "设置 header: %s: %s", key, val);
            esp_http_client_set_header(client, key, val);
        }
    }
}

/* ---------- 轻量 JSON 字符串提取（无需第三方库） ---------- */

/* 解析 4 位十六进制 */
static uint32_t json_hex4(const char *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4 && p[i]; i++) {
        char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else break;
    }
    return v;
}

/* 将 Unicode 码点按 UTF-8 追加到 out，返回新的写入位置 */
static int json_utf8_append(char *out, size_t out_len, int pos, uint32_t cp)
{
    if (cp < 0x80) {
        if (pos < (int)out_len - 1) out[pos++] = (char)cp;
    } else if (cp < 0x800) {
        if (pos < (int)out_len - 2) {
            out[pos++] = (char)(0xC0 | (cp >> 6));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (pos < (int)out_len - 3) {
            out[pos++] = (char)(0xE0 | (cp >> 12));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if (pos < (int)out_len - 4) {
            out[pos++] = (char)(0xF0 | (cp >> 18));
            out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return pos;
}

/**
 * @brief 从 JSON 文本中提取指定 key 的字符串值
 *
 * 支持常见转义（\" \\ \/ \n \r \t \b \f \uXXXX，含代理对）。找不到 key
 * 或值格式异常时返回 false，out 置空串。用于解析 image_url / title。
 */
static bool json_extract_string(const char *json, const char *key, char *out, size_t out_len)
{
    if (out_len == 0) return false;
    out[0] = '\0';

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *k = strstr(json, pattern);
    if (k == NULL) return false;

    const char *p = k + strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;

    int pos = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            switch (*p) {
            case 'n': if (pos < (int)out_len - 1) out[pos++] = '\n'; p++; break;
            case 'r': if (pos < (int)out_len - 1) out[pos++] = '\r'; p++; break;
            case 't': if (pos < (int)out_len - 1) out[pos++] = '\t'; p++; break;
            case 'b': if (pos < (int)out_len - 1) out[pos++] = '\b'; p++; break;
            case 'f': if (pos < (int)out_len - 1) out[pos++] = '\f'; p++; break;
            case 'u': {
                uint32_t cp = json_hex4(p + 1);
                p += 5;
                /* 处理 UTF-16 代理对 */
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    uint32_t lo = json_hex4(p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                pos = json_utf8_append(out, out_len, pos, cp);
                break;
            }
            default:
                if (pos < (int)out_len - 1) out[pos++] = *p;
                p++;
                break;
            }
        } else {
            if (pos < (int)out_len - 1) out[pos++] = *p;
            p++;
        }
    }

    out[pos] = '\0';
    return true;
}

esp_err_t image_fetcher_get_next_url(char *url_buf, size_t buf_len)
{
    /* 直接使用 backend_url 作为完整请求地址，不再自动拼接尾缀 */
    ESP_LOGI(TAG, "GET %s", s_backend_url);

    json_recv_ctx_t ctx = {0};

    esp_http_client_config_t config = {
        .url          = s_backend_url,
        .event_handler = json_http_event_cb,
        .user_data    = &ctx,
        .timeout_ms   = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 初始化失败");
        return ESP_FAIL;
    }

    apply_custom_headers(client);

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

    /* 提取 image_url（可能为空字符串：后端条目没有可用配图，此时用纯色背景） */
    url_buf[0] = '\0';
    if (json_extract_string(ctx.buf, "image_url", url_buf, buf_len)) {
        ESP_LOGI(TAG, "image_url: %s", url_buf[0] ? url_buf : "(空，使用纯色背景)");
    } else {
        ESP_LOGW(TAG, "JSON 中无 image_url 字段，使用纯色背景");
    }

    /* 提取 title（UTF-8，可能含中文；用于图片底部浮层） */
    if (!json_extract_string(ctx.buf, "title", s_current_title, sizeof(s_current_title))) {
        s_current_title[0] = '\0';
    }
    ESP_LOGI(TAG, "title: %s", s_current_title[0] ? s_current_title : "(空)");

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

/**
 * @brief 无图片/图片不可用时的纯色背景 + 标题浮层
 */
static esp_err_t image_fetcher_show_fallback(void)
{
    ESP_LOGI(TAG, "使用纯色背景 + 标题浮层（无可用配图）");
    ui_animation_show_solid_banner(SOLID_BG_RGB24, s_current_title);
    return ESP_OK;
}

esp_err_t image_fetcher_download_and_show(const char *url)
{
    /* 无图片 URL：直接纯色背景 + 标题浮层 */
    if (url == NULL || url[0] == '\0') {
        return image_fetcher_show_fallback();
    }

    ESP_LOGI(TAG, "下载图片: %s", url);

    /* 预排版标题浮层（在逐行写屏前准备字形，供 outfunc 叠加） */
    ui_animation_banner_prepare(s_current_title);

    /* 先 open 连接，获取 Content-Length，再按实际大小分配 buffer */
    esp_http_client_config_t config = {
        .url         = url,
        .timeout_ms  = 15000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 初始化失败");
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
    }

    apply_custom_headers(client);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open 失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP 状态码: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
    }

    /* 确定分配大小：Content-Length 未知时用上限，已知时按实际大小（不超上限） */
    size_t alloc_size = JPEG_BUF_MAX;
    if (content_len > 0 && content_len < (int)JPEG_BUF_MAX) {
        alloc_size = (size_t)content_len;
    } else if (content_len >= (int)JPEG_BUF_MAX) {
        ESP_LOGE(TAG, "Content-Length %d 超出上限 %d", content_len, (int)JPEG_BUF_MAX);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
    }

    uint8_t *jpeg_buf = (uint8_t *)malloc(alloc_size);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "malloc JPEG buffer 失败（需要 %d bytes，可用最大块 %d bytes）",
                 (int)alloc_size,
                 (int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
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
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
    }

    ESP_LOGI(TAG, "JPEG 下载完成，大小: %d bytes（分配: %d bytes）", total_read, (int)alloc_size);

    /* 分配 TJpgDec 工作内存池 */
    void *pool = malloc(JPEG_POOL_SIZE);
    if (!pool) {
        ESP_LOGE(TAG, "malloc TJpgDec 内存池失败");
        free(jpeg_buf);
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
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
        ui_animation_banner_release();
        return image_fetcher_show_fallback();
    }

    ESP_LOGI(TAG, "JPEG 尺寸: %dx%d，开始解码...", jdec.width, jdec.height);

    /* 解码（scale=0 即原始尺寸），outfunc 负责写屏并叠加标题浮层 */
    jres = jd_decomp(&jdec, jpeg_outfunc, 0);
    if (jres != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp 返回: %d（可能为正常中断）", jres);
    }

    free(pool);
    free(jpeg_buf);
    ui_animation_banner_release();
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
             "%s/v1/item/%" PRIu32 "/rating", s_backend_url, s_current_item_id);

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

    apply_custom_headers(client);

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

const char *image_fetcher_get_backend_url(void)
{
    return s_backend_url;
}

const char *image_fetcher_get_http_headers(void)
{
    if (s_http_headers[0] == '\0') return "[]";
    return s_http_headers;
}

const char *image_fetcher_get_title(void)
{
    return s_current_title;
}
