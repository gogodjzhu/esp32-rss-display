/**
 * @file main.cpp
 * @brief LVGL Benchmark Demo 入口（临时，用于测试显示性能）
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "ui_animation.h"
#include "lvgl.h"
#include "demos/lv_demos.h"

static const char *TAG = "MAIN";

// 程序入口
extern "C" void app_main()
{
    ESP_LOGI(TAG, "LVGL Benchmark Demo Starting...");

    // 初始化显示屏和 LVGL
    ui_animation_init();

    // 启动 benchmark demo
    lv_demo_benchmark();

    // 主循环：持续驱动 LVGL 渲染
    while (true) {
        ui_animation_task();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
