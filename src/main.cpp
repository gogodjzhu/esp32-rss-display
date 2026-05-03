/**
 * @file main.cpp
 * @brief 主入口 - WiFi 连接后定时从后端拉取图片显示到 TFT 屏幕
 *
 * 流程：
 *   1. 初始化显示屏（ui_animation_init）
 *   2. 初始化 WiFi 并连接（wifi_manager_smart_connect）
 *   3. 无凭证 → 进入 AP 配网模式，启动 HTTP server 提供配网页面，等待配网
 *   4. 连接成功 → 启动 HTTP 管理服务器 + image_display_task
 *   5. image_display_task 每 30 秒：拉取图片 URL → 下载并显示 → 底部进度条倒计时
 *   6. HTTP 管理页面在两种模式下均可访问
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "ui_animation.h"
#include "wifi_manager.h"
#include "image_fetcher.h"
#include "http_server.h"

static const char *TAG = "MAIN";

/* 图片 URL 缓冲区最大长度 */
#define IMAGE_URL_BUF_LEN 512

/* 每次图片展示后等待的时间（秒） */
#define DISPLAY_INTERVAL_SEC 180

/* 进度条更新间隔（毫秒），越小越平滑 */
#define PROGRESS_UPDATE_MS   100

/* ---------- 图片显示任务 ---------- */

static void image_display_task(void *pvParameters)
{
    (void)pvParameters;

    image_fetcher_init();
    ESP_LOGI(TAG, "图片显示任务启动");

    char url_buf[IMAGE_URL_BUF_LEN];

    while (true) {
        /* 获取下一条图片 URL */
        esp_err_t err = image_fetcher_get_next_url(url_buf, sizeof(url_buf));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "获取图片 URL 失败，5 秒后重试...");
            ui_animation_show_error();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 切换到图片模式：清除 spinner 等 LVGL 组件，预建底部进度条 */
        ui_animation_prepare_image_mode();

        /* 下载并显示图片 */
        err = image_fetcher_download_and_show(url_buf);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "图片下载/显示失败，5 秒后重试...");
            ui_animation_show_error();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 底部进度条平滑倒计时：每 100ms 更新一次，共 DISPLAY_INTERVAL_SEC * 10 步 */
        int total_steps = DISPLAY_INTERVAL_SEC * (1000 / PROGRESS_UPDATE_MS);
        for (int t = 0; t <= total_steps; t++) {
            int percent = t * 100 / total_steps;
            ui_animation_update_bottom_bar(percent);
            vTaskDelay(pdMS_TO_TICKS(PROGRESS_UPDATE_MS));
        }
    }
}

/* ---------- 程序入口 ---------- */

extern "C" void app_main()
{
    ESP_LOGI(TAG, "RSS 图片显示固件启动");

    /* 初始化显示屏和 LVGL */
    ui_animation_init();
    ui_animation_show_loading();

    /* 初始化 WiFi 管理器 */
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败");
        ui_animation_show_error();
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* 尝试连接 WiFi（使用 NVS 中保存的凭证，或进入 AP 配网模式） */
    err = wifi_manager_smart_connect();
    if (err != ESP_OK) {
        /* 无凭证，已进入 AP 配网模式（192.168.4.1）
         * 启动 HTTP server 提供配网页面，然后驱动 LVGL 等待用户配网 */
        ESP_LOGI(TAG, "进入 AP 配网模式，启动 HTTP 配网服务器");
        ui_animation_show_no_network();
        http_server_start();
        while (true) {
            ui_animation_task();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "WiFi 已连接，启动 HTTP 管理服务器");

    /* 启动 HTTP 管理服务器（提供 /、/api/status、/reset 等页面） */
    httpd_handle_t http_server = http_server_start();
    if (http_server == NULL) {
        ESP_LOGW(TAG, "HTTP 服务器启动失败，继续运行图片显示");
    }

    /* 启动图片显示任务（栈 8KB，优先级 5） */
    xTaskCreate(image_display_task, "img_display", 8192, NULL, 5, NULL);

    /* app_main 退出，FreeRTOS 调度器接管 */
    vTaskDelete(NULL);
}
