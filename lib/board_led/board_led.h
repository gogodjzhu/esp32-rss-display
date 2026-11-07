#ifndef BOARD_LED_H
#define BOARD_LED_H
#include <esp_err.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 板上指示 LED，初始化为固定电平 */
esp_err_t board_led_init(void);
#ifdef __cplusplus
}
#endif
#endif
