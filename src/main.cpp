#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "app_controller.h"
#include "ui_animation.h"

extern "C" void app_main()
{
    esp_err_t err = app_controller_run();
    ESP_LOGE("MAIN", "启动失败: %s", esp_err_to_name(err));
    ui_animation_show_error();
    while (true) {
        ui_animation_task();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
