#ifndef JPEG_DECODER_H
#define JPEG_DECODER_H
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef esp_err_t (*jpeg_row_fn)(void *context, int x, int y, uint16_t *pixels, int width);
esp_err_t jpeg_decoder_render(const uint8_t *data, size_t size, int width, int height,
                              jpeg_row_fn output, void *context);
#ifdef __cplusplus
}
#endif
#endif
