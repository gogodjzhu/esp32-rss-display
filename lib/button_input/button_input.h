#ifndef BUTTON_INPUT_H
#define BUTTON_INPUT_H
#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
esp_err_t button_input_init(void);
bool button_input_receive(int64_t *timestamp_ms);
unsigned button_input_dropped(void);
#endif
