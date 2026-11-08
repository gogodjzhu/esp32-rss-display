#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t display_driver_init(void);
/* 设置整屏 180° 翻转（设备倒装安装时使用）；必须在 display_driver_init() 之前调用 */
void display_driver_set_flip(bool flip_180);
/* RGB565 已交换字节序；调用结束后可以复用像素数据 */
esp_err_t display_driver_write(int x, int y, int width, int height, const uint16_t *pixels);
#ifdef __cplusplus
}
#endif
#endif
