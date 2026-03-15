/**
 * @file wifi_manager.c
 * @brief WiFi Manager 实现
 */

#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include "wifi_manager.h"

static const char *TAG = "WIFI_MGR";

#define WIFI_SSID     "shell"
#define WIFI_PASSWORD "11235813"

static esp_ip4_addr_t s_ip_addr;
static bool s_connected = false;

// WiFi事件处理函数
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        s_connected = false;
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_ip_addr = event->ip_info.ip;
        s_connected = true;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&s_ip_addr));
    }
}

esp_err_t wifi_manager_connect(wifi_work_mode_t mode)
{
    // 初始化NVS存储
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化TCP/IP协议栈
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化WiFi驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (mode == MODE_AP) {
        // 创建默认WiFi AP网络接口
        esp_netif_create_default_wifi_ap();

        // 注册事件处理函数
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

        // 配置WiFi AP
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.ap.ssid, WIFI_SSID, sizeof(wifi_config.ap.ssid));
        wifi_config.ap.ssid_len = strlen(WIFI_SSID);
        strncpy((char*)wifi_config.ap.password, WIFI_PASSWORD, sizeof(wifi_config.ap.password));
        wifi_config.ap.max_connection = 4;
        wifi_config.ap.authmode = WIFI_AUTH_WPA_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "WiFi AP started, SSID: %s", WIFI_SSID);
        return ESP_OK;
    }

    // 默认STA模式
    // 创建默认WiFi Station网络接口
    esp_netif_create_default_wifi_sta();

    // 注册事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 配置WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi connecting to SSID: %s...", WIFI_SSID);

    // 等待连接成功 (简化实现，实际应使用事件组)
    int count = 0;
    while (!s_connected && count < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        count++;
    }

    if (s_connected) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_ERR_TIMEOUT;
    }
}

esp_ip4_addr_t* wifi_manager_get_ip(void)
{
    return s_connected ? &s_ip_addr : NULL;
}
