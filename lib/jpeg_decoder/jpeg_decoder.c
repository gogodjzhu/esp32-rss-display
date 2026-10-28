#include "jpeg_decoder.h"
#include <rom/tjpgd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size, position;
    jpeg_row_fn output;
    void *context;
    esp_err_t error;
    uint16_t *row;
    int width, height;
    TickType_t last_yield;
} decode_context_t;

static UINT input(JDEC *decoder, BYTE *buffer, UINT size)
{
    decode_context_t *ctx = decoder->device;
    size_t remaining = ctx->size - ctx->position;
    if (size > remaining) size = remaining;
    if (buffer) memcpy(buffer, ctx->data + ctx->position, size);
    ctx->position += size;
    return size;
}

static UINT output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    decode_context_t *ctx = decoder->device;
    int width = rect->right - rect->left + 1;
    if (width > ctx->width || rect->right >= ctx->width || rect->bottom >= ctx->height) return 0;
    const uint8_t *source = bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t color = ((source[3*x] & 0xF8) << 8) | ((source[3*x+1] & 0xFC) << 3) | (source[3*x+2] >> 3);
            ctx->row[x] = (color >> 8) | (color << 8);
        }
        ctx->error = ctx->output(ctx->context, rect->left, y, ctx->row, width);
        if (ctx->error != ESP_OK) return 0;
        source += width * 3;
    }
    /* 限制连续解码占用 CPU 的时间，队列事件在整图结束后处理 */
    if (xTaskGetTickCount() - ctx->last_yield >= pdMS_TO_TICKS(20)) {
        vTaskDelay(1);
        ctx->last_yield = xTaskGetTickCount();
    }
    return 1;
}

esp_err_t jpeg_decoder_render(const uint8_t *data, size_t size, int width, int height,
                              jpeg_row_fn callback, void *context)
{
    if (!data || !size || width <= 0 || height <= 0 || !callback) return ESP_ERR_INVALID_ARG;
    void *pool = malloc(5120);
    uint16_t *row = malloc((size_t)width * sizeof(uint16_t));
    if (!pool || !row) { free(pool); free(row); return ESP_ERR_NO_MEM; }
    decode_context_t ctx = {.data = data, .size = size, .output = callback, .context = context,
        .row = row, .width = width, .height = height, .last_yield = xTaskGetTickCount()};
    JDEC decoder;
    JRESULT result = jd_prepare(&decoder, input, pool, 5120, &ctx);
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    /* 协议规定图片填满显示区域，拒绝尺寸不符的数据，防止越界和残留 */
    if (result == JDR_OK && decoder.width == width && decoder.height == height) {
        result = jd_decomp(&decoder, output, 0);
        err = ctx.error != ESP_OK ? ctx.error : result == JDR_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    free(row);
    free(pool);
    return err;
}
