#ifndef APP_CONFIG_H
#define APP_CONFIG_H
#include "config_model.h"
#include <esp_err.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/**
 * 启动时配置的加载结果。
 *
 * 前四项是正常路径；其余为降级路径：底层存储不可用或已保存内容无法使用。
 * 降级时保留原始 NVS 值，使用默认配置（不含 WiFi 凭证）启动配网入口，
 * 只有显式保存才会覆盖，绝不静默清除用户配置。
 */
typedef enum {
    APP_CONFIG_LOADED = 0,          /* 读取并使用已保存的 schema 1 配置 */
    APP_CONFIG_CREATED,             /* 首次启动，生成并保存默认配置 */
    APP_CONFIG_MIGRATED,            /* 从旧键迁移并保存 */
    APP_CONFIG_SAVED,               /* 本次运行中显式保存成功，重启后生效 */
    APP_CONFIG_INVALID_RETAINED,    /* 已保存内容损坏或版本不支持，原值保留 */
    APP_CONFIG_READ_FAILED,         /* 底层读取失败，未写入任何内容 */
    APP_CONFIG_LEGACY_INVALID,      /* 旧地址无法识别，旧键原样保留 */
    APP_CONFIG_STORAGE_UNAVAILABLE, /* 存储不可用或写入失败，默认配置仅内存有效 */
    APP_CONFIG_STATE_COUNT
} app_config_state_t;

typedef struct {
    app_config_state_t state;
    /* 无法识别的旧地址原文，仅用于诊断展示，不写入新配置 */
    char legacy_invalid_url[CONFIG_URL_MAX];
} app_config_status_t;

esp_err_t app_config_init(void);
void app_config_get_active(app_config_t *out);
void app_config_get_saved(app_config_t *out);
void app_config_get_status(app_config_status_t *out);
const char *app_config_state_name(app_config_state_t state);
bool app_config_is_degraded(void);
esp_err_t app_config_save(const app_config_t *config);
esp_err_t app_config_save_wifi(const char *ssid, const char *password);
esp_err_t app_config_clear_wifi(void);
#ifdef __cplusplus
}
#endif
#endif
