/**
 * @file main.cpp
 * @brief ESP32 WiFi 配置入口
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "wifi_manager.h"
#include "http_server.h"
#include "rss_reader.h"

static const char *TAG = "MAIN";

// 程序入口
extern "C" void app_main()
{
    ESP_LOGI(TAG, "ESP32 WiFi Config Portal Starting...");

    // 初始化WiFi Manager
    wifi_manager_init();

    // 检查是否有保存的WiFi凭证
    if (wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Found WiFi credentials, starting STA mode...");
        // 有保存的凭证, 启动STA模式尝试连接
        wifi_manager_start_sta_mode();
    } else {
        ESP_LOGI(TAG, "No WiFi credentials found, starting AP mode...");
        // 无保存的凭证, 直接启动AP配置模式
        wifi_manager_start_ap_mode();
    }

    // 获取WiFi信息并打印当前状态
    wifi_info_t *info = wifi_manager_get_info();
    if (info) {
        if (info->mode == MODE_AP) {
            ESP_LOGI(TAG, "Running in AP mode, SSID: %s", info->ap_ssid);
            ESP_LOGI(TAG, "Access the config page at http://%s", info->ip);
        } else {
            ESP_LOGI(TAG, "WiFi connecting in background, waiting for IP...");
        }
    }

    // 启动HTTP服务器, 提供配网页面和管理页面
    httpd_handle_t server = http_server_start();
    if (server == NULL) {
        ESP_LOGE(TAG, "HTTP server start failed!");
    }

    rss_reader_init();
    xTaskCreate(rss_reader_task, "rss_reader", 8192, NULL, 3, NULL);

    // 10分钟超时检测计时
    int64_t start_time = esp_timer_get_time() / 1000;
    int64_t timeout_ms = 600000;  // 10分钟

    // 主循环 - 监控连接状态并在超时时切换到AP模式
    while (true) {
        wifi_info_t *info = wifi_manager_get_info();
        
        // 如果处于STA模式但未连接, 检查是否超时
        if (info->mode == MODE_STA && info->status != WIFI_STATUS_CONNECTED) {
            int64_t elapsed = esp_timer_get_time() / 1000 - start_time;
            
            // 超过10分钟仍未连接成功, 切换到AP模式
            if (elapsed >= timeout_ms && info->status != WIFI_STATUS_CONNECTED) {
                ESP_LOGW(TAG, "Connection timeout after %ld seconds, switching to AP mode", (long)(elapsed / 1000));
                
                // 停止当前WiFi
                esp_wifi_stop();
                vTaskDelay(pdMS_TO_TICKS(500));
                
                // 切换到AP模式
                wifi_manager_start_ap_mode();
                
                ESP_LOGI(TAG, "AP mode started, SSID: %s", info->ap_ssid);
                ESP_LOGI(TAG, "Access the config page at http://%s", info->ip);
            }
        }
        
        // 打印当前状态日志
        ESP_LOGI(TAG, "Status: %d, IP: %s, Free heap: %ld bytes", 
                 info->status, info->ip, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
