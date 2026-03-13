/**
 * @file main.cpp
 * @brief ESP32-C3 HTTP服务器示例程序入口
 * 
 * 功能：连接WiFi后启动HTTP服务器，返回"Hello World"
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_log.h>

#include "wifi_manager.h"
#include "http_server.h"

static const char *TAG = "MAIN";

extern "C" void app_main()
{
    // 连接WiFi (wifi_manager内部处理所有初始化)
    if (wifi_manager_connect() == ESP_OK) {
        // 获取并打印IP地址
        esp_ip4_addr_t* ip = wifi_manager_get_ip();
        if (ip) {
            ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(ip));
        }
    } else {
        ESP_LOGE(TAG, "WiFi connection failed!");
    }

    // 启动HTTP服务器
    httpd_handle_t server = http_server_start();
    if (server == NULL) {
        ESP_LOGE(TAG, "HTTP server start failed!");
    }

    // 主循环
    while (true) {
        ESP_LOGI(TAG, "Free heap: %ld bytes", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
