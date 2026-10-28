#ifndef CONFIG_MODEL_H
#define CONFIG_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define APP_CONFIG_SCHEMA 1
#define CONFIG_URL_MAX 256
#define CONFIG_DEVICE_MAX 64
#define CONFIG_HEADERS_MAX 512

typedef struct {
    uint32_t schema_version;
    char api_base_url[CONFIG_URL_MAX];
    char device_id[CONFIG_DEVICE_MAX];
    char http_headers[CONFIG_HEADERS_MAX];
    char wifi_ssid[33];
    char wifi_password[65];
    /* 无法可靠识别的旧地址原样保留，等待管理页面修正 */
    char legacy_next_url[CONFIG_URL_MAX];
} app_config_t;

bool config_model_validate(const app_config_t *config);
bool config_model_parse(const char *json, app_config_t *out);
char *config_model_json(const app_config_t *config, bool include_wifi);
bool config_model_migrate_url(app_config_t *config, const char *legacy_url);
bool config_model_next_url(const app_config_t *config, char *out, size_t size);
bool config_model_rating_url(const app_config_t *config, uint32_t item_id, char *out, size_t size);
bool config_model_http_url(const char *url);
/* 由芯片 MAC 生成出厂设备标识，保证多台设备不共用同一测试身份 */
bool config_model_default_device_id(const uint8_t mac[6], char *out, size_t size);
#ifdef __cplusplus
}
#endif
#endif
