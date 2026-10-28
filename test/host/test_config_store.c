#include "app_config.h"
#include "nvs_manager.h"
#include <nvs.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 以单个字符串模拟 NVS 提交，失败时保留原值 */
static char persisted[4096];
static bool fail_save, fail_read;
static unsigned writes;
static const char *legacy_url;
esp_err_t nvs_manager_init(void) { return ESP_OK; }
esp_err_t nvs_manager_get_str(const char *key, char *out, size_t *length)
{
    if (fail_read) return ESP_FAIL;
    const char *value = NULL;
    if (!strcmp(key, "config_v1") && persisted[0]) value = persisted;
    if (legacy_url) {
        if (!strcmp(key, "backend_url")) value = legacy_url;
        if (!strcmp(key, "wifi_ssid")) value = "old-wifi";
        if (!strcmp(key, "wifi_password")) value = "old-password";
        if (!strcmp(key, "http_headers")) value = "[{\"k\":\"X-Test\",\"v\":\"legacy\"}]";
    }
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    size_t required = strlen(value) + 1;
    if (out && *length < required) return ESP_ERR_INVALID_SIZE;
    if (out) memcpy(out, value, required);
    *length = required;
    return ESP_OK;
}
esp_err_t nvs_manager_set_str(const char *key, const char *value)
{
    assert(!strcmp(key, "config_v1") && strlen(value) < sizeof(persisted));
    if (fail_save) return ESP_FAIL;
    strcpy(persisted, value);
    writes++;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    app_config_t active, saved;
    if (!strcmp(argv[1], "migration")) legacy_url = "https://backend/prefix/v1/device/old-device/next";
    else if (!strcmp(argv[1], "custom")) legacy_url = "http://backend/custom?x=1";
    else if (!strcmp(argv[1], "corrupt")) strcpy(persisted, "{broken");
    else if (!strcmp(argv[1], "read-failure")) {
        fail_read = true;
        assert(app_config_init() == ESP_FAIL && !writes);
        return 0;
    } else if (!strcmp(argv[1], "saved")) {
        app_config_t config = {.schema_version = APP_CONFIG_SCHEMA,
            .api_base_url = "https://persisted", .device_id = "saved-device", .http_headers = "[]", .wifi_ssid = "saved-wifi"};
        char *json = config_model_json(&config, true);
        assert(json);
        strcpy(persisted, json);
        free(json);
    } else assert(!strcmp(argv[1], "defaults"));
    assert(app_config_init() == ESP_OK);
    app_config_get_active(&active);
    app_config_get_saved(&saved);
    assert(config_model_validate(&active));
    if (legacy_url) {
        assert(!strcmp(active.wifi_ssid, "old-wifi") && !strcmp(active.wifi_password, "old-password"));
        if (!strcmp(argv[1], "migration")) {
            assert(!strcmp(active.api_base_url, "https://backend/prefix") && !strcmp(active.device_id, "old-device"));
        } else assert(!strcmp(active.legacy_next_url, legacy_url) && !active.api_base_url[0]);
        assert(writes == 1);
    } else if (!strcmp(argv[1], "corrupt")) {
        assert(!strcmp(persisted, "{broken") && !writes && !active.wifi_ssid[0]);
    } else if (!strcmp(argv[1], "saved")) {
        assert(!strcmp(active.device_id, "saved-device") && !strcmp(active.wifi_ssid, "saved-wifi") && !writes);
    } else assert(!strcmp(active.device_id, "default-device") && writes == 1);

    assert(app_config_save_wifi("new-wifi", "new-password") == ESP_OK);
    /* 后端更新基于旧快照时，也必须保留最新保存的凭证 */
    strcpy(saved.api_base_url, "http://new-backend");
    saved.legacy_next_url[0] = 0;
    assert(app_config_save(&saved) == ESP_OK);
    app_config_get_saved(&saved);
    assert(!strcmp(saved.wifi_ssid, "new-wifi") && !strcmp(saved.wifi_password, "new-password"));
    app_config_t still_active;
    app_config_get_active(&still_active);
    assert(!strcmp(still_active.api_base_url, active.api_base_url) && !strcmp(still_active.wifi_ssid, active.wifi_ssid));
    char before[4096];
    strcpy(before, persisted);
    fail_save = true;
    assert(app_config_save_wifi("lost-wifi", "lost-password") == ESP_FAIL);
    app_config_get_saved(&saved);
    assert(!strcmp(saved.wifi_ssid, "new-wifi") && !strcmp(persisted, before));
    fail_save = false;
    assert(app_config_clear_wifi() == ESP_OK);
    app_config_get_saved(&saved);
    assert(!saved.wifi_ssid[0] && !saved.wifi_password[0]);
    assert(config_model_parse(persisted, &saved) && !saved.wifi_ssid[0]);
    printf("PASS: config %s, atomic save failures, saved/active isolation, WiFi reset\n", argv[1]);
    return 0;
}
