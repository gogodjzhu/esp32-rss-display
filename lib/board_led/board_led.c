#include "board_led.h"
#include "board_config.h"
#include <driver/gpio.h>

esp_err_t board_led_init(void)
{
    gpio_config_t pins = {.pin_bit_mask = 1ULL << BOARD_LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&pins);
    if (err == ESP_OK) gpio_set_level(BOARD_LED_GPIO, 0); /* 板上 LED 固定拉低 */
    return err;
}
