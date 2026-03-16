#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "nvs_manager.h"

static const char *TAG = "NVS_MGR";

#define NVS_NAMESPACE "app_config"
#define KEY_FIRMWARE_VER "fw_ver"

static bool s_initialized = false;

esp_err_t nvs_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition full or version mismatch, erasing and re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Checking firmware version (current=%u)", CONFIG_FIRMWARE_VERSION);
    
    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        uint32_t saved_ver = 0;
        esp_err_t err = nvs_get_u32(nvs_handle, KEY_FIRMWARE_VER, &saved_ver);
        nvs_close(nvs_handle);
        
        if (err == ESP_OK && saved_ver != CONFIG_FIRMWARE_VERSION) {
            ESP_LOGW(TAG, "Firmware version changed (%u -> %u), clearing NVS", 
                     saved_ver, CONFIG_FIRMWARE_VERSION);
            nvs_manager_erase_all();
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG, "Firmware version unchanged (%u)", saved_ver);
        } else {
            ESP_LOGI(TAG, "No saved firmware version, writing %u", CONFIG_FIRMWARE_VERSION);
        }
        
        if (err != ESP_OK || saved_ver != CONFIG_FIRMWARE_VERSION) {
            nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
            nvs_set_u32(nvs_handle, KEY_FIRMWARE_VER, CONFIG_FIRMWARE_VERSION);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NVS Manager initialized");
    return ESP_OK;
}

esp_err_t nvs_manager_set_str(const char *key, const char *value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs_handle, key, value);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_get_str(const char *key, char *out_value, size_t *length)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(nvs_handle, key, out_value, length);
    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_set_u32(const char *key, uint32_t value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(nvs_handle, key, value);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_get_u32(const char *key, uint32_t *out_value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u32(nvs_handle, key, out_value);
    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_erase(const char *key)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs_handle, key);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_manager_erase_all(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "All NVS data erased");
    return err;
}

bool nvs_manager_has_key(const char *key)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;
    }
    
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs_handle, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_get_u32(nvs_handle, key, NULL);
    }
    nvs_close(nvs_handle);
    
    return err == ESP_OK;
}
