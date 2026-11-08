#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "本固件仅支持 ESP32-S3"
#endif
#define BOARD_TFT_MOSI 2
#define BOARD_TFT_SCLK 1
#define BOARD_TFT_CS 5
#define BOARD_TFT_DC 4
#define BOARD_TFT_RST 3
#define BOARD_BUTTON_GPIO 0
#define BOARD_LED_GPIO 48
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240
#define DISPLAY_BAR_HEIGHT 2
/* 面板原生 MADCTL：0x60 = MX|MV 行列交换，把 240x320 竖排面板横排使用。
 * 其他固件只按各自面板调整这一个值，翻转推导对任何原生方向都成立。 */
#define BOARD_DISPLAY_MADCTL 0x60
/* 180° 翻转只取反 MX|MY 方向位（0x80|0x40），行列交换与 RGB 顺序位保持原生定义 */
#define BOARD_DISPLAY_MADCTL_FLIP_MASK 0xC0
/* 板卡实际写盘的 MADCTL：flip_180 为真时整屏旋转 180°（设备倒装安装） */
static inline uint8_t board_display_madctl(bool flip_180)
{
    return flip_180 ? (uint8_t)(BOARD_DISPLAY_MADCTL ^ BOARD_DISPLAY_MADCTL_FLIP_MASK)
                    : BOARD_DISPLAY_MADCTL;
}
#endif
