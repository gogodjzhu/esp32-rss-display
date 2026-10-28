#include "app_config.h"
#include "nvs_manager.h"
#include "sdkconfig.h"
#include <nvs.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

static SemaphoreHandle_t s_mutex;
static app_config_t s_active;
static app_config_t s_saved;
static bool s_ready;

static esp_err_t persist(const app_config_t *config)
{
    if (!config_model_validate(config)) return ESP_ERR_INVALID_ARG;
    char *json = config_model_json(config, true);
    if (!json) return ESP_ERR_NO_MEM;
    /* 一个 NVS 值承载整份配置，避免多个 key 部分更新 */
    esp_err_t err = nvs_manager_set_str("config_v1", json);
    free(json);
    if (err == ESP_OK) s_saved = *config;
    return err;
}

esp_err_t app_config_init(void)
{
    if (s_ready) return ESP_OK;
    esp_err_t err = nvs_manager_init();
    if (err != ESP_OK) return err;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    app_config_t config = {
        .schema_version = APP_CONFIG_SCHEMA,
        .api_base_url = CONFIG_BACKEND_BASE_URL,
        .device_id = CONFIG_DEVICE_ID,
        .http_headers = "[]",
    };
    size_t length = 0;
    err = nvs_manager_get_str("config_v1", NULL, &length);
    if (err == ESP_OK) {
        bool valid = false;
        if (length && length <= 4096) {
            char *json = malloc(length);
            if (!json) return ESP_ERR_NO_MEM;
            err = nvs_manager_get_str("config_v1", json, &length);
            valid = err == ESP_OK && config_model_parse(json, &config);
            free(json);
            if (err != ESP_OK) return err;
        }
        if (!valid) {
            /* 保留原值，使用无凭证默认配置开放修复入口，显式保存后才覆盖 */
            ESP_LOGW("CONFIG", "已保存配置无效或版本不支持，使用默认配置等待修复");
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        char legacy[CONFIG_URL_MAX];
        length = sizeof(legacy);
        err = nvs_manager_get_str("backend_url", legacy, &length);
        if (err == ESP_OK && legacy[0]) {
            if (!config_model_migrate_url(&config, legacy)) return ESP_ERR_INVALID_ARG;
        } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        length = sizeof(config.http_headers);
        err = nvs_manager_get_str("http_headers", config.http_headers, &length);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        length = sizeof(config.wifi_ssid);
        err = nvs_manager_get_str("wifi_ssid", config.wifi_ssid, &length);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        length = sizeof(config.wifi_password);
        err = nvs_manager_get_str("wifi_password", config.wifi_password, &length);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        err = persist(&config);
        if (err != ESP_OK) return err;
    } else return err;
    if (!config_model_validate(&config)) return ESP_ERR_INVALID_ARG;
    s_active = s_saved = config;
    s_ready = true;
    return ESP_OK;
}

void app_config_get_active(app_config_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_active;
    xSemaphoreGive(s_mutex);
}

void app_config_get_saved(app_config_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_saved;
    xSemaphoreGive(s_mutex);
}

esp_err_t app_config_save(const app_config_t *config)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* 管理 API 更新后端设置时保留最新已保存的 WiFi 凭证 */
    app_config_t next = *config;
    memcpy(next.wifi_ssid, s_saved.wifi_ssid, sizeof(next.wifi_ssid));
    memcpy(next.wifi_password, s_saved.wifi_password, sizeof(next.wifi_password));
    esp_err_t err = persist(&next);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t app_config_save_wifi(const char *ssid, const char *password)
{
    if (!s_ready || !ssid || !password || !*ssid || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    app_config_t next = s_saved;
    strcpy(next.wifi_ssid, ssid);
    strcpy(next.wifi_password, password);
    esp_err_t err = persist(&next);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t app_config_clear_wifi(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    app_config_t next = s_saved;
    next.wifi_ssid[0] = next.wifi_password[0] = '\0';
    esp_err_t err = persist(&next);
    xSemaphoreGive(s_mutex);
    return err;
}
