#include "display_driver.h"
#include "board_config.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

static spi_device_handle_t s_spi;

static esp_err_t send_data(bool command, const void *data, size_t size)
{
    gpio_set_level(BOARD_TFT_DC, command ? 0 : 1);
    spi_transaction_t transaction = {.length = size * 8, .tx_buffer = data};
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t command(uint8_t value)
{
    return send_data(true, &value, 1);
}

esp_err_t display_driver_init(void)
{
    if (s_spi) return ESP_OK;
    gpio_config_t pins = {.pin_bit_mask = (1ULL << BOARD_TFT_RST) | (1ULL << BOARD_TFT_DC), .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&pins);
    if (err != ESP_OK) return err;
    gpio_set_level(BOARD_TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    spi_bus_config_t bus = {.mosi_io_num = BOARD_TFT_MOSI, .miso_io_num = -1, .sclk_io_num = BOARD_TFT_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4092};
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t device = {.clock_speed_hz = 40000000, .mode = 0, .spics_io_num = BOARD_TFT_CS, .queue_size = 1};
    err = spi_bus_add_device(SPI2_HOST, &device, &s_spi);
    if (err != ESP_OK) { spi_bus_free(SPI2_HOST); return err; }
    if ((err = command(0x01)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));
    if ((err = command(0x11)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t orientation = 0x60;
    if ((err = command(0x36)) != ESP_OK || (err = send_data(false, &orientation, 1)) != ESP_OK) return err;
    uint8_t format = 0x55;
    if ((err = command(0x3A)) != ESP_OK || (err = send_data(false, &format, 1)) != ESP_OK) return err;
    if ((err = command(0x29)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t display_driver_write(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (!s_spi || !pixels || x < 0 || y < 0 || width <= 0 || height <= 0
        || width > DISPLAY_WIDTH - x || height > DISPLAY_HEIGHT - y) return ESP_ERR_INVALID_ARG;
    uint16_t x2 = x + width - 1, y2 = y + height - 1;
    uint8_t columns[] = {x >> 8, x, x2 >> 8, x2};
    uint8_t rows[] = {y >> 8, y, y2 >> 8, y2};
    esp_err_t err;
    if ((err = command(0x2A)) != ESP_OK || (err = send_data(false, columns, sizeof(columns))) != ESP_OK
        || (err = command(0x2B)) != ESP_OK || (err = send_data(false, rows, sizeof(rows))) != ESP_OK
        || (err = command(0x2C)) != ESP_OK) return err;
    size_t remaining = (size_t)width * height * 2;
    const uint8_t *data = (const uint8_t *)pixels;
    while (remaining) {
        size_t size = remaining > 4092 ? 4092 : remaining;
        if ((err = send_data(false, data, size)) != ESP_OK) return err;
        data += size;
        remaining -= size;
    }
    return ESP_OK;
}
