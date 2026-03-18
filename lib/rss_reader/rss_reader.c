/**
 * @file rss_reader.c
 * @brief RSS Reader 实现 - 流式解析RSS XML，随机选取一条缓存
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_random.h>
#include "rss_reader.h"
#include "sdkconfig.h"

static const char *TAG = "RSS_READER";

#define RSS_URL              CONFIG_RSS_URL
#define RSS_FETCH_INTERVAL_S CONFIG_RSS_FETCH_INTERVAL_SEC

#define MAX_ITEMS            50

typedef struct {
    char titles[MAX_ITEMS][RSS_ITEM_TITLE_MAX];
    char links[MAX_ITEMS][RSS_ITEM_LINK_MAX];
    char pubDates[MAX_ITEMS][RSS_ITEM_DATE_MAX];
    int count;
} rss_items_t;

static rss_cached_item_t cached_item = {0};
static rss_items_t items = {0};
static bool has_items = false;

/**
 * @brief RSS XML流式解析器状态机
 *
 * 解析器状态用于追踪XML解析的当前位置:
 * - PARSING_NONE: 不在任何标签内，解析 <item> 等顶层标签
 * - PARSING_ITEM: 在 <item> ... </item> 内部
 * - PARSING_TITLE: 在 <title> ... </title> 内部，累积文本
 * - PARSING_LINK: 在 <link> ... </link> 内部，累积文本
 * - PARSING_PUBDATE: 在 <pubDate> ... </pubDate> 内部，累积文本
 */
typedef enum {
    PARSING_NONE,
    PARSING_ITEM,
    PARSING_TITLE,
    PARSING_LINK,
    PARSING_PUBDATE,
} parser_state_t;

static parser_state_t parser_state = PARSING_NONE;

/**
 * @brief 缓冲区操作宏 - 向目标缓冲区追加字符，自动截断并维护\0
 *
 * @param buf 目标缓冲区
 * @param max_len 缓冲区最大长度
 * @param c 要追加的字符
 * @param pos 当前写入位置的指针（追加后递增）
 */
#define BUF_PUSH(buf, max_len, c, pos) do { \
    if ((pos) < (max_len) - 1) { \
        (buf)[(pos)++] = (c); \
        (buf)[(pos)] = '\0'; \
    } \
} while (0)

/**
 * @brief 缓冲区清零宏
 *
 * @param buf 要清零的缓冲区
 * @param max_len 缓冲区最大长度
 */
#define BUF_CLEAR(buf, max_len) do { \
    memset((buf), 0, (max_len)); \
} while (0)

/**
 * @brief HTML实体解码
 *
 * 将HTML编码的字符串转换为实际字符，例如:
 *   &lt;   -> <
 *   &gt;   -> >
 *   &amp;  -> &
 *   &quot;  -> "
 *   &apos; -> '
 *
 * @param str 输入输出字符串（原地解码）
 */
static void html_decode(char *str)
{
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '&') {
            /* 检查常见HTML实体 */
            if (strncmp(src, "&lt;", 4) == 0) {
                *dst++ = '<';
                src += 4;
            } else if (strncmp(src, "&gt;", 4) == 0) {
                *dst++ = '>';
                src += 4;
            } else if (strncmp(src, "&amp;", 5) == 0) {
                *dst++ = '&';
                src += 5;
            } else if (strncmp(src, "&quot;", 6) == 0) {
                *dst++ = '"';
                src += 6;
            } else if (strncmp(src, "&apos;", 6) == 0) {
                *dst++ = '\'';
                src += 6;
            } else if (src[1] == '#') {
                /* 处理数字字符实体 &#nnn; 或 &#xnn; */
                char *p = src + 2;
                int code = 0;
                if (*p == 'x' || *p == 'X') {
                    /* 十六进制格式 &#xnnn; */
                    p++;
                    while (*p && *p != ';' && isxdigit((unsigned char)*p)) {
                        code = code * 16 + (isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10);
                        p++;
                    }
                } else {
                    /* 十进制格式 &#nnn; */
                    while (*p && *p != ';' && isdigit((unsigned char)*p)) {
                        code = code * 10 + (*p - '0');
                        p++;
                    }
                }
                if (*p == ';') p++;
                if (code > 0 && code <= 0x10FFFF) {
                    *dst++ = (char)code;
                }
                src = p;
            } else {
                /* 非实体 & 符号，保留原样 */
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief 去除字符串首尾空白字符
 *
 * @param str 要处理的字符串
 */
static void trim_string(char *str)
{
    char *start = str;
    char *end;

    /* 跳过前端空白 */
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    /* 字符串全为空白 */
    if (*start == '\0') {
        str[0] = '\0';
        return;
    }

    /* 定位末尾非空白字符 */
    end = str + strlen(str) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }
    *(end + 1) = '\0';

    /* 如果有前导空白，移动字符串 */
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/**
 * @brief HTTP事件回调 - 流式XML解析
 *
 * ESP-IDF的esp_http_client会分多次调用此回调，每次传递一个数据块。
 * 解析器通过状态机累加标签内容，跨chunk边界时状态保持不变，
 * 保证完整解析XML标签内容。
 *
 * @param evt HTTP客户端事件
 * @return ESP_OK 成功
 */
static esp_err_t xml_parse_cb(esp_http_client_event_t *evt)
{
    /* 各字段的累积缓冲区和写入位置 */
    static char title_buf[RSS_ITEM_TITLE_MAX] = {0};
    static int title_pos = 0;
    static char link_buf[RSS_ITEM_LINK_MAX] = {0};
    static int link_pos = 0;
    static char pubDate_buf[RSS_ITEM_DATE_MAX] = {0};
    static int pubDate_pos = 0;

    /* 当前正在解析的标签名缓冲区（支持跨chunk累积） */
    static char current_tag[32] = {0};
    static int tag_pos = 0;
    static bool in_tag = false;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA: {
        /* 已达到最大item数量，跳过剩余数据 */
        if (items.count >= MAX_ITEMS) break;

        char *data = (char *)evt->data;
        int len = evt->data_len;

        for (int i = 0; i < len; i++) {
            char c = data[i];

            if (c == '<') {
                /* 开始一个新标签 */
                in_tag = true;
                tag_pos = 0;
                current_tag[0] = '\0';
            } else if (c == '>') {
                /* 标签结束 - 解析标签名并决定动作 */
                in_tag = false;
                current_tag[tag_pos] = '\0';

                /* 判断是否为闭合标签（以/开头） */
                bool is_closing = (tag_pos > 0 && current_tag[0] == '/');
                const char *tag_name = is_closing ? (current_tag + 1) : current_tag;

                if (strcasecmp(tag_name, "item") == 0) {
                    if (!is_closing) {
                        /* <item> 开始 - 重置所有字段缓冲区 */
                        parser_state = PARSING_ITEM;
                        BUF_CLEAR(title_buf, sizeof(title_buf));
                        BUF_CLEAR(link_buf, sizeof(link_buf));
                        BUF_CLEAR(pubDate_buf, sizeof(pubDate_buf));
                        title_pos = 0;
                        link_pos = 0;
                        pubDate_pos = 0;
                    } else {
                        /* </item> 结束 - 保存累积的条目 */
                        parser_state = PARSING_NONE;

                        /* HTML解码所有字段（处理 &lt; &gt; 等实体） */
                        html_decode(title_buf);
                        html_decode(link_buf);
                        html_decode(pubDate_buf);
                        trim_string(title_buf);
                        trim_string(link_buf);
                        trim_string(pubDate_buf);

                        /* 仅保存有标题的条目 */
                        if (title_buf[0] != '\0' && items.count < MAX_ITEMS) {
                            strncpy(items.titles[items.count], title_buf, RSS_ITEM_TITLE_MAX - 1);
                            strncpy(items.links[items.count], link_buf, RSS_ITEM_LINK_MAX - 1);
                            strncpy(items.pubDates[items.count], pubDate_buf, RSS_ITEM_DATE_MAX - 1);
                            items.titles[items.count][RSS_ITEM_TITLE_MAX - 1] = '\0';
                            items.links[items.count][RSS_ITEM_LINK_MAX - 1] = '\0';
                            items.pubDates[items.count][RSS_ITEM_DATE_MAX - 1] = '\0';
                            items.count++;
                        }
                    }
                } else if (parser_state == PARSING_ITEM) {
                    /* 在<item>内部，检查子标签 */
                    if (!is_closing) {
                        if (strcasecmp(tag_name, "title") == 0) {
                            parser_state = PARSING_TITLE;
                            BUF_CLEAR(title_buf, sizeof(title_buf));
                            title_pos = 0;
                        } else if (strcasecmp(tag_name, "link") == 0) {
                            parser_state = PARSING_LINK;
                            BUF_CLEAR(link_buf, sizeof(link_buf));
                            link_pos = 0;
                        } else if (strcasecmp(tag_name, "pubdate") == 0) {
                            parser_state = PARSING_PUBDATE;
                            BUF_CLEAR(pubDate_buf, sizeof(pubDate_buf));
                            pubDate_pos = 0;
                        }
                    } else {
                        /* 闭合子标签，恢复到item状态 */
                        if (parser_state == PARSING_TITLE ||
                            parser_state == PARSING_LINK ||
                            parser_state == PARSING_PUBDATE) {
                            parser_state = PARSING_ITEM;
                        }
                    }
                }
            } else if (in_tag) {
                /* 正在解析标签名，累积字符（忽略属性部分，但标签名在<之后、空白之前） */
                if (tag_pos < (int)(sizeof(current_tag) - 1)) {
                    /* 标签名在遇到空白或 /> 时结束，
                     * 这里只累积到空白前的字符 */
                    if (!isspace((unsigned char)c)) {
                        current_tag[tag_pos++] = c;
                        current_tag[tag_pos] = '\0';
                    }
                }
            } else {
                /* 不在标签内，是文本内容 - 追加到对应缓冲区 */
                switch (parser_state) {
                case PARSING_TITLE:
                    BUF_PUSH(title_buf, sizeof(title_buf), c, title_pos);
                    break;
                case PARSING_LINK:
                    BUF_PUSH(link_buf, sizeof(link_buf), c, link_pos);
                    break;
                case PARSING_PUBDATE:
                    BUF_PUSH(pubDate_buf, sizeof(pubDate_buf), c, pubDate_pos);
                    break;
                default:
                    break;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

/**
 * @brief 从RSS URL获取并解析RSS feed
 *
 * @return ESP_OK 成功解析到至少一个条目
 */
static esp_err_t fetch_and_parse_rss(void)
{
    /* 重置解析状态和条目缓冲区 */
    memset(&items, 0, sizeof(items));
    parser_state = PARSING_NONE;

    esp_http_client_config_t config = {
        .url = RSS_URL,
        .event_handler = xml_parse_cb,
        .timeout_ms = 15000,
    };

    ESP_LOGI(TAG, "Fetching RSS from: %s", RSS_URL);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    /* 执行HTTP GET请求，触发xml_parse_cb多次回调 */
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status: %d, fetched %d items", status, items.count);

    esp_http_client_cleanup(client);

    if (items.count == 0) {
        ESP_LOGW(TAG, "No items parsed from RSS feed");
        return ESP_FAIL;
    }

    has_items = true;
    return ESP_OK;
}

/**
 * @brief 从解析到的条目中随机选取一条
 */
static void pick_random_item(void)
{
    if (!has_items || items.count == 0) {
        cached_item.valid = false;
        return;
    }

    /* 使用ESP32硬件随机数选取条目索引 */
    int idx = esp_random() % items.count;
    strncpy(cached_item.title, items.titles[idx], RSS_ITEM_TITLE_MAX - 1);
    strncpy(cached_item.link, items.links[idx], RSS_ITEM_LINK_MAX - 1);
    strncpy(cached_item.pubDate, items.pubDates[idx], RSS_ITEM_DATE_MAX - 1);
    cached_item.title[RSS_ITEM_TITLE_MAX - 1] = '\0';
    cached_item.link[RSS_ITEM_LINK_MAX - 1] = '\0';
    cached_item.pubDate[RSS_ITEM_DATE_MAX - 1] = '\0';
    cached_item.valid = true;

    ESP_LOGI(TAG, "Picked item %d/%d: %s", idx + 1, items.count, cached_item.title);
}

/**
 * @brief 初始化RSS Reader模块
 */
esp_err_t rss_reader_init(void)
{
    ESP_LOGI(TAG, "RSS Reader initialized (URL: %s, interval: %ds)",
             RSS_URL, RSS_FETCH_INTERVAL_S);
    memset(&cached_item, 0, sizeof(cached_item));
    cached_item.valid = false;
    return ESP_OK;
}

/**
 * @brief RSS Reader FreeRTOS任务
 *
 * 启动后延迟2秒等待WiFi连接建立，
 * 然后每隔RSS_FETCH_INTERVAL_S秒获取一次RSS feed。
 */
void rss_reader_task(void *pvParameters)
{
    (void)pvParameters;

    /* 等待系统初始化完成 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (true) {
        if (fetch_and_parse_rss() == ESP_OK) {
            pick_random_item();
        } else {
            ESP_LOGW(TAG, "Failed to fetch RSS, will retry in %ds...", RSS_FETCH_INTERVAL_S);
        }

        vTaskDelay(pdMS_TO_TICKS(RSS_FETCH_INTERVAL_S * 1000));
    }
}

/**
 * @brief 获取当前缓存的RSS条目
 */
rss_cached_item_t* rss_reader_get_cached_item(void)
{
    return &cached_item;
}
