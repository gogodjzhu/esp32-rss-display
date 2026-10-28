#include "app_config.h"
#include "nvs_manager.h"
#include <esp_mac.h>
#include <nvs.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 以单个字符串模拟 NVS 提交，失败时保留原值 */
static char persisted[4096];
static bool fail_save, fail_read, fail_init;
static unsigned writes;
static const char *legacy_url, *legacy_headers;

/* 固定的芯片 MAC，用于验证出厂设备标识的生成与保存 */
static const uint8_t test_mac[6] = {0x00, 0x11, 0x22, 0xaa, 0xbb, 0xcc};

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    assert(type == ESP_MAC_WIFI_STA);
    memcpy(mac, test_mac, sizeof(test_mac));
    return ESP_OK;
}

esp_err_t nvs_manager_init(void) { return fail_init ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_manager_get_str(const char *key, char *out, size_t *length)
{
    if (fail_read || fail_init) return ESP_FAIL;
    const char *value = NULL;
    if (!strcmp(key, "config_v1") && persisted[0]) value = persisted;
    if (legacy_url) {
        if (!strcmp(key, "backend_url")) value = legacy_url;
        if (!strcmp(key, "wifi_ssid")) value = "old-wifi";
        if (!strcmp(key, "wifi_password")) value = "old-password";
        if (!strcmp(key, "http_headers")) value = legacy_headers ? legacy_headers : "[{\"k\":\"X-Test\",\"v\":\"legacy\"}]";
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
    if (fail_save || fail_init) return ESP_FAIL;
    strcpy(persisted, value);
    writes++;
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    app_config_t active, saved;
    app_config_status_t status;

    if (!strcmp(scenario, "migration")) legacy_url = "https://backend/prefix/v1/device/old-device/next";
    else if (!strcmp(scenario, "custom")) legacy_url = "http://backend/custom?x=1";
    else if (!strcmp(scenario, "legacy-invalid")) legacy_url = "ftp://backend/feed";
    else if (!strcmp(scenario, "legacy-bad-headers")) {
        legacy_url = "http://backend";
        legacy_headers = "not-json";
    } else if (!strcmp(scenario, "corrupt")) strcpy(persisted, "{broken");
    else if (!strcmp(scenario, "read-failure")) fail_read = true;
    else if (!strcmp(scenario, "nvs-init-failure")) fail_init = true;
    else if (!strcmp(scenario, "saved")) {
        app_config_t config = {.schema_version = APP_CONFIG_SCHEMA,
            .api_base_url = "https://persisted", .device_id = "saved-device", .http_headers = "[]", .wifi_ssid = "saved-wifi"};
        char *json = config_model_json(&config, true);
        assert(json);
        strcpy(persisted, json);
        free(json);
    } else assert(!strcmp(scenario, "defaults"));

    /* 降级路径同样返回 ESP_OK：设备以默认配置开放修复入口，而不是停在错误画面 */
    assert(app_config_init() == ESP_OK);
    app_config_get_active(&active);
    app_config_get_saved(&saved);
    app_config_get_status(&status);
    assert(config_model_validate(&active) && config_model_validate(&saved));

    if (!strcmp(scenario, "defaults")) {
        /* 首次启动生成并保存由 MAC 派生的设备标识，不共享编译期测试身份 */
        assert(status.state == APP_CONFIG_CREATED && !app_config_is_degraded());
        assert(!strcmp(active.device_id, "esp32s3-001122aabbcc"));
        assert(strstr(persisted, "\"device_id\":\"esp32s3-001122aabbcc\""));
        assert(!active.wifi_ssid[0] && writes == 1);
    } else if (!strcmp(scenario, "migration")) {
        assert(status.state == APP_CONFIG_MIGRATED);
        assert(!strcmp(active.api_base_url, "https://backend/prefix") && !strcmp(active.device_id, "old-device"));
        assert(!strcmp(active.wifi_ssid, "old-wifi") && !strcmp(active.wifi_password, "old-password"));
        assert(writes == 1);
    } else if (!strcmp(scenario, "custom")) {
        /* 无法可靠识别但合法的旧地址：作为 legacy_next_url 保留并可继续使用，评分不可用 */
        assert(status.state == APP_CONFIG_MIGRATED && !app_config_is_degraded());
        assert(!strcmp(active.legacy_next_url, legacy_url) && !active.api_base_url[0]);
        assert(!strcmp(active.wifi_ssid, "old-wifi") && writes == 1);
    } else if (!strcmp(scenario, "legacy-invalid")) {
        assert(status.state == APP_CONFIG_LEGACY_INVALID && !strcmp(status.legacy_invalid_url, legacy_url));
        assert(!writes && !active.wifi_ssid[0] && app_config_is_degraded());
    } else if (!strcmp(scenario, "legacy-bad-headers")) {
        /* 旧值本身不合法：旧键原样保留，默认配置可用 */
        assert(status.state == APP_CONFIG_INVALID_RETAINED && !writes && app_config_is_degraded());
        assert(!active.wifi_ssid[0]);
    } else if (!strcmp(scenario, "corrupt")) {
        assert(status.state == APP_CONFIG_INVALID_RETAINED && !strcmp(persisted, "{broken") && !writes);
        assert(!active.wifi_ssid[0] && app_config_is_degraded());
    } else if (!strcmp(scenario, "read-failure")) {
        assert(status.state == APP_CONFIG_READ_FAILED && !writes && app_config_is_degraded());
        assert(!active.wifi_ssid[0]);
    } else if (!strcmp(scenario, "nvs-init-failure")) {
        assert(status.state == APP_CONFIG_STORAGE_UNAVAILABLE && !writes && app_config_is_degraded());
        assert(!strcmp(active.device_id, "esp32s3-001122aabbcc"));
    } else if (!strcmp(scenario, "saved")) {
        assert(status.state == APP_CONFIG_LOADED && !app_config_is_degraded());
        assert(!strcmp(active.device_id, "saved-device") && !strcmp(active.wifi_ssid, "saved-wifi") && !writes);
    }

    if (fail_init) {
        /* 存储不可用时保存必须如实失败，不能报告成功 */
        assert(app_config_save_wifi("new-wifi", "new-password") == ESP_FAIL);
        assert(app_config_clear_wifi() == ESP_FAIL);
        app_config_get_saved(&saved);
        assert(!saved.wifi_ssid[0] && !persisted[0]);
        printf("PASS: config %s keeps defaults in memory and reports save failure\n", scenario);
        return 0;
    }

    /* 显式保存是唯一的覆盖路径：降级状态在一次成功保存后回到可用状态 */
    assert(app_config_save_wifi("new-wifi", "new-password") == ESP_OK);
    app_config_get_status(&status);
    assert(status.state == APP_CONFIG_SAVED && !app_config_is_degraded());
    /* 修复保存后不再保留旧地址诊断信息 */
    assert(!status.legacy_invalid_url[0]);
    app_config_get_active(&active);
    assert(strcmp(active.wifi_ssid, "new-wifi"));
    app_config_t active_before = active;

    /* 后端更新基于旧快照时，也必须保留最新保存的凭证 */
    app_config_get_saved(&saved);
    strcpy(saved.api_base_url, "http://new-backend");
    saved.legacy_next_url[0] = 0;
    assert(app_config_save(&saved) == ESP_OK);
    app_config_get_saved(&saved);
    assert(!strcmp(saved.wifi_ssid, "new-wifi") && !strcmp(saved.wifi_password, "new-password"));
    app_config_t still_active;
    app_config_get_active(&still_active);
    assert(!strcmp(still_active.api_base_url, active_before.api_base_url)
        && !strcmp(still_active.wifi_ssid, active_before.wifi_ssid));
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
    printf("PASS: config %s, explicit recovery save, atomic failures, saved/active isolation, WiFi reset\n", scenario);
    return 0;
}
