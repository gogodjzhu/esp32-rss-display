#include "app_config.h"
#include "nvs_manager.h"
#include "sdkconfig.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "CONFIG";
/* 一个 NVS 值承载整份配置，避免多个 key 部分更新 */
#define CONFIG_STORE_KEY "config_v1"
#define CONFIG_STORE_MAX 4096
/* 旧键只在没有新结构配置时读取；新配置存在后不再回读 */
static const char *const LEGACY_URL_KEY = "backend_url";

static SemaphoreHandle_t s_mutex;
static app_config_t s_active;
static app_config_t s_saved;
static app_config_status_t s_status;
static bool s_ready;

static const char *const s_state_names[APP_CONFIG_STATE_COUNT] = {
    "loaded", "created", "migrated", "saved",
    "invalid_retained", "read_failed", "legacy_invalid", "storage_unavailable",
};

const char *app_config_state_name(app_config_state_t state)
{
    return (unsigned)state < APP_CONFIG_STATE_COUNT ? s_state_names[state] : "unknown";
}

bool app_config_is_degraded(void)
{
    if (!s_mutex) return s_status.state >= APP_CONFIG_INVALID_RETAINED;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool degraded = s_status.state >= APP_CONFIG_INVALID_RETAINED;
    xSemaphoreGive(s_mutex);
    return degraded;
}

void app_config_get_status(app_config_status_t *out)
{
    if (!out) return;
    if (!s_mutex) { *out = s_status; return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

static void set_state(app_config_state_t state)
{
    s_status.state = state;
    /* 只有降级状态才保留旧地址诊断信息，恢复或保存后清除 */
    if (state < APP_CONFIG_INVALID_RETAINED) s_status.legacy_invalid_url[0] = '\0';
}

/* 写入整份配置；成功后才更新 saved 快照，失败保留原值和原状态 */
static esp_err_t persist(const app_config_t *config)
{
    if (!config_model_validate(config)) return ESP_ERR_INVALID_ARG;
    char *json = config_model_json(config, true);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_manager_set_str(CONFIG_STORE_KEY, json);
    free(json);
    if (err == ESP_OK) s_saved = *config;
    return err;
}

/* 编译期标识仅作为显式覆盖；留空时由芯片 MAC 生成唯一标识 */
static bool initial_device_id(app_config_t *config)
{
    if (CONFIG_DEVICE_ID[0]) {
        if (strlen(CONFIG_DEVICE_ID) >= sizeof(config->device_id)) return false;
        strcpy(config->device_id, CONFIG_DEVICE_ID);
        return true;
    }
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
    return config_model_default_device_id(mac, config->device_id, sizeof(config->device_id));
}

/* 无新结构配置时导入旧键。旧值不可用或读取失败时保留旧键并降级，不写入任何内容 */
static esp_err_t import_legacy(app_config_t *config, app_config_status_t *status)
{
    app_config_t candidate = *config;
    char legacy[CONFIG_URL_MAX] = "";
    size_t length = sizeof(legacy);
    esp_err_t err = nvs_manager_get_str(LEGACY_URL_KEY, legacy, &length);
    if (err == ESP_OK && legacy[0]) {
        if (!config_model_migrate_url(&candidate, legacy)) {
            /* 非法旧地址：原文只留作诊断，不写入新结构，也不覆盖旧键 */
            strncpy(status->legacy_invalid_url, legacy, sizeof(status->legacy_invalid_url) - 1);
            status->legacy_invalid_url[sizeof(status->legacy_invalid_url) - 1] = '\0';
            status->state = APP_CONFIG_LEGACY_INVALID;
            return ESP_OK;
        }
    } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        status->state = APP_CONFIG_READ_FAILED;
        return ESP_OK;
    }
    const struct { const char *key; char *out; size_t size; } values[] = {
        {"http_headers", candidate.http_headers, sizeof(candidate.http_headers)},
        {"wifi_ssid", candidate.wifi_ssid, sizeof(candidate.wifi_ssid)},
        {"wifi_password", candidate.wifi_password, sizeof(candidate.wifi_password)},
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        length = values[i].size;
        err = nvs_manager_get_str(values[i].key, values[i].out, &length);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            status->state = APP_CONFIG_READ_FAILED;
            return ESP_OK;
        }
    }
    if (!config_model_validate(&candidate)) {
        /* 旧值本身不合法：保留旧键，使用默认配置等待显式保存 */
        status->state = APP_CONFIG_INVALID_RETAINED;
        return ESP_OK;
    }
    if ((err = persist(&candidate)) != ESP_OK) {
        ESP_LOGE(TAG, "迁移结果写入失败: %s", esp_err_to_name(err));
        status->state = APP_CONFIG_STORAGE_UNAVAILABLE;
        return ESP_OK;
    }
    *config = candidate;
    status->state = legacy[0] ? APP_CONFIG_MIGRATED : APP_CONFIG_CREATED;
    return ESP_OK;
}

/* 读取已保存的新结构配置；返回错误只表示内存等本地故障 */
static esp_err_t load_saved(app_config_t *config, app_config_status_t *status)
{
    size_t length = 0;
    esp_err_t err = nvs_manager_get_str(CONFIG_STORE_KEY, NULL, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) return import_legacy(config, status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取已保存配置失败: %s", esp_err_to_name(err));
        status->state = APP_CONFIG_READ_FAILED;
        return ESP_OK;
    }
    if (!length || length > CONFIG_STORE_MAX) {
        status->state = APP_CONFIG_INVALID_RETAINED;
        return ESP_OK;
    }
    char *json = malloc(length);
    if (!json) return ESP_ERR_NO_MEM;
    err = nvs_manager_get_str(CONFIG_STORE_KEY, json, &length);
    app_config_t loaded = *config;
    bool valid = err == ESP_OK && config_model_parse(json, &loaded);
    free(json);
    if (err != ESP_OK) status->state = APP_CONFIG_READ_FAILED;
    else if (!valid) status->state = APP_CONFIG_INVALID_RETAINED;
    else {
        *config = loaded;
        status->state = APP_CONFIG_LOADED;
    }
    return ESP_OK;
}

esp_err_t app_config_init(void)
{
    if (s_ready) return ESP_OK;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    app_config_t config = {
        .schema_version = APP_CONFIG_SCHEMA,
        .api_base_url = CONFIG_BACKEND_BASE_URL,
        .http_headers = "[]",
    };
    if (!initial_device_id(&config)) return ESP_ERR_INVALID_STATE;
    app_config_status_t status = {.state = APP_CONFIG_LOADED, .legacy_invalid_url = ""};
    esp_err_t err = nvs_manager_init();
    if (err != ESP_OK) {
        /* 存储不可用时仍启动配网入口：默认配置只在内存有效，保存会如实返回错误 */
        ESP_LOGE(TAG, "存储初始化失败: %s", esp_err_to_name(err));
        status.state = APP_CONFIG_STORAGE_UNAVAILABLE;
    } else if ((err = load_saved(&config, &status)) != ESP_OK) return err;
    if (!config_model_validate(&config)) return ESP_ERR_INVALID_ARG;
    s_active = s_saved = config;
    s_status = status;
    s_ready = true;
    if (app_config_is_degraded()) {
        ESP_LOGW(TAG, "配置降级为 %s，已保存内容保持不变，等待显式保存",
                 app_config_state_name(s_status.state));
    } else {
        ESP_LOGI(TAG, "配置状态 %s，设备标识 %s", app_config_state_name(s_status.state), s_active.device_id);
    }
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
    if (err == ESP_OK) set_state(APP_CONFIG_SAVED);
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
    if (err == ESP_OK) set_state(APP_CONFIG_SAVED);
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
    if (err == ESP_OK) set_state(APP_CONFIG_SAVED);
    xSemaphoreGive(s_mutex);
    return err;
}
