#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H
#include <esp_err.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t display_driver_init(void);
/* RGB565 已交换字节序；调用结束后可以复用像素数据 */
esp_err_t display_driver_write(int x, int y, int width, int height, const uint16_t *pixels);
#ifdef __cplusplus
}
#endif
#endif
