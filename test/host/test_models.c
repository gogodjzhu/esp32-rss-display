#include "app_state.h"
#include "config_model.h"
#include "content_item.h"
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

static void test_content(void)
{
    content_item_t item;
    assert(content_item_parse("{\"item_id\":42,\"title\":\"新闻 \\u4e2d\\u6587 \\\"quote\\\"\",\"image_url\":\"https://host/image.jpg\",\"published_at\":\"2026-10-28 10:00\"}", &item));
    assert(item.id == 42 && strstr(item.title, "中文"));
    char banner[512];
    content_item_banner(&item, banner, sizeof(banner));
    assert(!strncmp(banner, "2026-10-28 10:00\n", 17));
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
    test_config();
    test_content();
    puts("PASS: gestures, rating, clocks, migration, configuration, content");
    return 0;
}
