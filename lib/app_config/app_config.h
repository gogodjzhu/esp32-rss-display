#ifndef APP_CONFIG_H
#define APP_CONFIG_H
#include "config_model.h"
#include <esp_err.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t app_config_init(void);
void app_config_get_active(app_config_t *out);
void app_config_get_saved(app_config_t *out);
esp_err_t app_config_save(const app_config_t *config);
esp_err_t app_config_save_wifi(const char *ssid, const char *password);
esp_err_t app_config_clear_wifi(void);
#ifdef __cplusplus
}
#endif
#endif
