#include "button_input.h"
#include "board_config.h"
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static QueueHandle_t s_queue;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_dropped;

static void button_isr(void *arg)
{
    (void)arg;
    static int64_t last_us = -50000;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 50000) return;
    last_us = now;
    int64_t timestamp_ms = now / 1000;
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &timestamp_ms, &woken) != pdTRUE) {
        portENTER_CRITICAL_ISR(&s_lock);
        s_dropped++;
        portEXIT_CRITICAL_ISR(&s_lock);
    }
    portYIELD_FROM_ISR(woken);
}

esp_err_t button_input_init(void)
{
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(16, sizeof(int64_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    gpio_config_t pins = {.pin_bit_mask = 1ULL << BOARD_BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE};
    esp_err_t err = gpio_config(&pins);
    if (err == ESP_OK) err = gpio_install_isr_service(0);
    if (err == ESP_OK) err = gpio_isr_handler_add(BOARD_BUTTON_GPIO, button_isr, NULL);
    if (err != ESP_OK) { vQueueDelete(s_queue); s_queue = NULL; }
    return err;
}

bool button_input_receive(int64_t *timestamp_ms)
{
    return s_queue && xQueueReceive(s_queue, timestamp_ms, 0) == pdTRUE;
}

unsigned button_input_dropped(void)
{
    portENTER_CRITICAL(&s_lock);
    unsigned value = s_dropped;
    portEXIT_CRITICAL(&s_lock);
    return value;
}
