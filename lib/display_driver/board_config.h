#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H
#include "sdkconfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "本固件仅支持 ESP32-S3"
#endif
#define BOARD_TFT_MOSI 2
#define BOARD_TFT_SCLK 1
#define BOARD_TFT_CS 6
#define BOARD_TFT_DC 5
#define BOARD_TFT_RST 4
#define BOARD_BUTTON_GPIO 0
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240
#define DISPLAY_BAR_HEIGHT 2
#endif
