#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_log.h>

static const char *TAG = "MAIN";

extern "C" void app_main() {
    while (true) {
        ESP_LOGI(TAG, "Running on ESP32 with FreeRTOS! Heap size: %ld bytes", esp_get_free_heap_size());
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
