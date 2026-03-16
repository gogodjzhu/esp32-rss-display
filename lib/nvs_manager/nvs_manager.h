#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_manager_init(void);

esp_err_t nvs_manager_set_str(const char *key, const char *value);
esp_err_t nvs_manager_get_str(const char *key, char *out_value, size_t *length);
esp_err_t nvs_manager_set_u32(const char *key, uint32_t value);
esp_err_t nvs_manager_get_u32(const char *key, uint32_t *out_value);
esp_err_t nvs_manager_erase(const char *key);
esp_err_t nvs_manager_erase_all(void);

bool nvs_manager_has_key(const char *key);

#ifdef __cplusplus
}
#endif

#endif
