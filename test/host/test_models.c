#include "app_state.h"
#include "config_model.h"
#include "content_item.h"
#include "text_utf8.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_config_t defaults(void)
{
    return (app_config_t){.schema_version = APP_CONFIG_SCHEMA,
        .api_base_url = "http://backend:8080", .device_id = "device-01", .http_headers = "[]"};
}

static void test_gestures(void)
{
    app_state_t state;
    app_state_shown(&state, 7, 1000);
    assert(app_state_button(&state, 1100) == APP_ACTION_NONE);
    assert(app_state_tick(&state, 1499) == APP_ACTION_NONE);
    assert(app_state_button(&state, 1499) == APP_ACTION_RATING);
    assert(state.item_id == 7 && state.rating == 0 && state.remaining_ms == 59501);
    assert(app_state_tick(&state, 6498) == APP_ACTION_NONE);
    assert(app_state_tick(&state, 6499) == APP_ACTION_RESTORE);
    assert(state.display_deadline == 66000);

    /* 400 毫秒边界按单击处理，慢请求期间重复请求合并 */
    app_state_shown(&state, 8, 0);
    app_state_button(&state, 100);
    assert(app_state_button(&state, 500) == APP_ACTION_NEXT);
    assert(app_state_tick(&state, 100000) == APP_ACTION_NONE);
    assert(state.item_id == 8 && state.next_requested);

    app_state_shown(&state, 9, 0);
    app_state_button(&state, 100);
    assert(app_state_tick(&state, 499) == APP_ACTION_NONE);
    assert(app_state_tick(&state, 500) == APP_ACTION_NEXT);
}

static void test_rating(void)
{
    app_state_t state;
    app_state_shown(&state, 42, 0);
    app_state_button(&state, 1000);
    assert(app_state_button(&state, 1200) == APP_ACTION_RATING);
    assert(app_state_button(&state, 1399) == APP_ACTION_NONE);
    assert(app_state_button(&state, 1400) == APP_ACTION_RATING && state.rating == 1);
    assert(app_state_tick(&state, 6399) == APP_ACTION_NONE);
    assert(app_state_tick(&state, 6400) == APP_ACTION_SUBMIT);
    assert(state.item_id == 42 && state.view == APP_SUBMITTING);
    assert(app_state_button(&state, 10000) == APP_ACTION_NONE);
    assert(app_state_tick(&state, 20000) == APP_ACTION_NONE);
    app_state_rating_done(&state, 20000);
    assert(app_state_tick(&state, 20999) == APP_ACTION_NONE);
    assert(app_state_tick(&state, 21000) == APP_ACTION_RESTORE);
    assert(state.display_deadline == 79800);
    assert(app_state_progress(&state, 21000) == 2);
    app_state_rating_done(&state, 30000);
    assert(state.view == APP_NORMAL);

    app_state_shown(&state, 1, 0);
    app_state_button(&state, 10);
    app_state_button(&state, 20);
    for (int i = 1; i <= 6; i++) {
        assert(app_state_button(&state, 20 + i * 200) == APP_ACTION_RATING);
        assert(state.rating == i % 6);
    }
    assert(app_state_tick(&state, 6220) == APP_ACTION_RESTORE);
}

static void test_clock(void)
{
    app_state_t fast, slow;
    /* 单调时钟可能已超过 32 位毫秒范围 */
    const int64_t start = INT64_C(5000000000);
    app_state_shown(&fast, 1, start);
    app_state_shown(&slow, 1, start);
    for (int64_t t = start; t < start + 60000; t += 40) assert(app_state_tick(&fast, t) == APP_ACTION_NONE);
    assert(app_state_tick(&slow, start + 59999) == APP_ACTION_NONE);
    assert(app_state_tick(&fast, start + 60000) == APP_ACTION_NEXT);
    assert(app_state_tick(&slow, start + 60000) == APP_ACTION_NEXT);
    assert(app_state_progress(&fast, start - 1) == 0);
    assert(app_state_progress(&fast, start + 100000) == 100);
    /* 无评分 ID 的内容仍有完整的轮播生命周期 */
    app_state_shown(&slow, 0, start);
    assert(slow.view == APP_NORMAL);
    assert(app_state_tick(&slow, start + 60000) == APP_ACTION_NEXT);
}

static void test_migration(void)
{
    app_config_t config = defaults();
    char url[512];
    assert(config_model_migrate_url(&config, "https://backend/prefix/v1/device/old-02/next"));
    assert(!strcmp(config.api_base_url, "https://backend/prefix"));
    assert(!strcmp(config.device_id, "old-02"));
    assert(config_model_next_url(&config, url, sizeof(url)));
    assert(!strcmp(url, "https://backend/prefix/v1/device/old-02/next"));
    assert(config_model_rating_url(&config, 123, url, sizeof(url)));
    assert(!strcmp(url, "https://backend/prefix/v1/item/123/rating"));
    assert(!config_model_rating_url(&config, 0, url, sizeof(url)));
    assert(!config_model_next_url(&config, url, 8));
    assert(config_model_migrate_url(&config, "http://backend/"));
    assert(!strcmp(config.api_base_url, "http://backend"));
    const char *unknown[] = {"http://backend/custom?token=x", "http://backend?token=x", "http://backend/v1/device/a/next?x=1"};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        assert(config_model_migrate_url(&config, unknown[i]));
        assert(config_model_validate(&config));
        assert(!config.api_base_url[0] && !strcmp(config.legacy_next_url, unknown[i]));
        assert(config_model_next_url(&config, url, sizeof(url)) && !strcmp(url, unknown[i]));
        assert(!config_model_rating_url(&config, 42, url, sizeof(url)));
    }
    assert(!config_model_migrate_url(&config, "ftp://backend/a"));
    config = defaults();
    strcpy(config.api_base_url, "http://backend/prefix///");
    assert(config_model_next_url(&config, url, sizeof(url)));
    assert(!strcmp(url, "http://backend/prefix/v1/device/device-01/next"));
}

static void test_device_id(void)
{
    /* 出厂标识由 MAC 派生：两台不同设备不会得到同一身份 */
    uint8_t first[6] = {0x00, 0x11, 0x22, 0xaa, 0xbb, 0xcc};
    uint8_t second[6] = {0x00, 0x11, 0x22, 0xaa, 0xbb, 0xcd};
    char id[CONFIG_DEVICE_MAX], other[CONFIG_DEVICE_MAX];
    assert(config_model_default_device_id(first, id, sizeof(id)));
    assert(!strcmp(id, "esp32s3-001122aabbcc"));
    assert(config_model_default_device_id(second, other, sizeof(other)) && strcmp(id, other) != 0);
    /* 缓存足够写入，但预留长度不足时失败，避免截断成另一个身份 */
    char narrow[16];
    assert(!config_model_default_device_id(first, narrow, sizeof(narrow)));
    app_config_t config = defaults();
    strcpy(config.device_id, id);
    assert(config_model_validate(&config));
    char url[512];
    assert(config_model_next_url(&config, url, sizeof(url)));
    assert(!strcmp(url, "http://backend:8080/v1/device/esp32s3-001122aabbcc/next"));
    /* 显式标识保持原样，不会因生成规则变化被改写 */
    config = defaults();
    assert(!strcmp(config.device_id, "device-01"));
}

static void test_config(void)
{
    app_config_t config = defaults(), parsed = defaults();
    strcpy(config.wifi_ssid, "中文网络\"\\");
    strcpy(config.wifi_password, "pass\"\\word");
    strcpy(config.http_headers, "[{\"k\":\"Authorization\",\"v\":\"Bearer example\"}]");
    char *json = config_model_json(&config, true);
    assert(json && config_model_parse(json, &parsed));
    assert(!memcmp(&config, &parsed, sizeof(config)));
    free(json);
    json = config_model_json(&config, false);
    assert(json && !strstr(json, "wifi_password") && !strstr(json, "wifi_ssid"));
    free(json);
    const char *bad_headers[] = {"{}", "[{}]", "[{\"k\":\"Host\",\"v\":\"x\"}]",
        "[{\"k\":\"X-Test\",\"v\":\"a\\r\\nb\"}]", "[{\"k\":\"X:Bad\",\"v\":\"a\"}]"};
    for (size_t i = 0; i < sizeof(bad_headers) / sizeof(bad_headers[0]); i++) {
        strcpy(config.http_headers, bad_headers[i]);
        assert(!config_model_validate(&config));
    }
    config = defaults();
    strcpy(config.device_id, "../bad/id");
    assert(!config_model_validate(&config));
    config = defaults();
    memset(config.api_base_url, 'x', sizeof(config.api_base_url));
    assert(!config_model_validate(&config));
    config = defaults();
    strcpy(config.wifi_password, "short");
    assert(!config_model_validate(&config));
    memset(config.wifi_password, 'a', 64);
    config.wifi_password[64] = 0;
    assert(config_model_validate(&config));
    config.wifi_password[0] = 'z';
    assert(!config_model_validate(&config));
    app_config_t before = parsed;
    assert(!config_model_parse("{\"schema_version\":2}", &parsed));
    assert(!memcmp(&before, &parsed, sizeof(before)));
    assert(!config_model_parse("{} trailing", &parsed));
    const char *bad_urls[] = {"http://", "http:///path", "http://user:pass@host", "http://host/has space", "file:///a", "http://host\\evil"};
    for (size_t i = 0; i < sizeof(bad_urls) / sizeof(bad_urls[0]); i++) assert(!config_model_http_url(bad_urls[i]));
}

static void test_stock_banner(void)
{
    content_item_t item;
    assert(content_item_parse("{\"item_id\":1,\"stocks\":["
        "{\"name\":\"上证指数\",\"change_pct\":0.85},"
        "{\"name\":\"NASDAQ\",\"change_pct\":-1.23},"
        "{\"name\":\"BTC\",\"change_pct\":0}]}", &item));
    assert(item.stock_count == 3);
    assert(item.stocks[0].change_bp == 85);
    assert(item.stocks[1].change_bp == -123);
    assert(item.stocks[2].change_bp == 0);
    /* 模型保留完整名称，显示裁剪由 banner 按像素宽度处理 */
    assert(!strcmp(item.stocks[0].name, "上证指数"));
    assert(!strcmp(item.stocks[1].name, "NASDAQ"));

    /* 行情是可选字段：非法条目丢弃、超限截断，都不能让整条内容失败 */
    const char *tolerated[] = {
        "{\"item_id\":1,\"title\":\"T\",\"stocks\":{}}",
        "{\"item_id\":1,\"title\":\"T\",\"stocks\":[{\"name\":\"A\"}]}",
        "{\"item_id\":1,\"title\":\"T\",\"stocks\":[{\"name\":\"A\",\"change_pct\":\"1\"}]}",
        "{\"item_id\":1,\"title\":\"T\",\"stocks\":[{\"name\":\"\",\"change_pct\":1}]}",
        "{\"item_id\":1,\"title\":\"T\",\"stocks\":[{},{}]}",
        "{\"item_id\":1,\"title\":\"T\",\"stocks\":null}",
    };
    for (size_t i = 0; i < sizeof(tolerated) / sizeof(tolerated[0]); i++) {
        assert(content_item_parse(tolerated[i], &item));
        assert(item.stock_count == 0 && !strcmp(item.title, "T"));
    }

    /* 非法条目夹在合法条目之间时逐条跳过，保持原始顺序 */
    assert(content_item_parse("{\"item_id\":1,\"stocks\":["
        "{\"name\":\"\",\"change_pct\":1},"
        "{\"name\":\"B\",\"change_pct\":2},"
        "{\"name\":\"C\",\"change_pct\":\"x\"},"
        "{\"name\":\"D\",\"change_pct\":3}]}", &item));
    assert(item.stock_count == 2);
    assert(!strcmp(item.stocks[0].name, "B") && item.stocks[0].change_bp == 200);
    assert(!strcmp(item.stocks[1].name, "D") && item.stocks[1].change_bp == 300);

    /* 超过 3 项按顺序截断，不视为协议错误 */
    assert(content_item_parse("{\"item_id\":1,\"stocks\":["
        "{\"name\":\"A\",\"change_pct\":1},{\"name\":\"B\",\"change_pct\":1},"
        "{\"name\":\"C\",\"change_pct\":1},{\"name\":\"D\",\"change_pct\":1}]}", &item));
    assert(item.stock_count == 3 && !strcmp(item.stocks[2].name, "C"));

    /* 名称超过存储上限时按完整码点截断，不留下半个 UTF-8 字符 */
    assert(content_item_parse("{\"item_id\":1,\"stocks\":["
        "{\"name\":\"非常长的股票名称测试中文\",\"change_pct\":1}]}", &item));
    assert(item.stock_count == 1 && !strcmp(item.stocks[0].name, "非常长的股票名称测试"));
}

static void test_text_utf8(void)
{
    const char *text = "中A";
    const char *p = text;
    assert(text_utf8_next(&p) == 0x4E2D && p - text == 3);
    assert(text_utf8_is_cjk(0x4E2D));
    assert(!text_utf8_is_cjk('A'));
    assert(text_utf8_is_space(' ') && text_utf8_is_space('\t'));
    assert(text_utf8_next(&p) == 'A' && *p == '\0');

    char clipped[CONTENT_STOCK_NAME_MAX];
    text_utf8_copy("上证指数", clipped, sizeof(clipped));
    assert(!strcmp(clipped, "上证指数"));
    text_utf8_copy("", clipped, sizeof(clipped));
    assert(clipped[0] == '\0');

    /* 缓冲不足时不能截断出半个 UTF-8 码点 */
    char tiny[4];
    text_utf8_copy("上证指数", tiny, sizeof(tiny));
    assert(!strcmp(tiny, "上"));
    text_utf8_copy("上证指数", tiny, 2);
    assert(tiny[0] == '\0');
    text_utf8_copy("A", tiny, 2);
    assert(!strcmp(tiny, "A"));
}

static void test_content(void)
{
    content_item_t item;
    assert(content_item_parse("{\"item_id\":42,\"title\":\"新闻 \\u4e2d\\u6587 \\\"quote\\\"\",\"image_url\":\"https://host/image.jpg\",\"published_at\":\"2026-10-28 10:00\"}", &item));
    assert(item.id == 42 && strstr(item.title, "中文"));
    const char *bad[] = {"{", "[]", "{\"item_id\":0}", "{\"item_id\":-1}", "{\"item_id\":1.5}",
        "{\"item_id\":4294967296}", "{\"item_id\":\"42\"}", "{\"item_id\":42,\"image_url\":\"file:///x\"}",
        "{\"item_id\":42,\"title\":true}", "{\"item_id\":42} trailing"};
    content_item_t before = item;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!content_item_parse(bad[i], &item));
        assert(!memcmp(&item, &before, sizeof(item)));
    }
    char oversized[512];
    memset(oversized, 'a', sizeof(oversized));
    memcpy(oversized, "{\"title\":\"", 10);
    memcpy(oversized + sizeof(oversized) - 3, "\"}", 3);
    assert(!content_item_parse(oversized, &item));
    assert(content_item_parse("{\"title\":\"legacy\",\"image_url\":null}", &item));
    assert(!item.id && !strcmp(item.title, "legacy") && !item.image_url[0]);
    assert(content_item_parse("{\"item_id\":4294967295}", &item) && item.id == UINT32_MAX);
}

int main(void)
{
    test_gestures();
    test_rating();
    test_clock();
    test_migration();
    test_device_id();
    test_config();
    test_content();
    test_text_utf8();
    test_stock_banner();
    puts("PASS: gestures, rating, clocks, migration, device identity, configuration, content, UTF-8 text, stock banner");
    return 0;
}
