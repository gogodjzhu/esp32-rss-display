#include "content_view.h"
#include "ui_animation.h"
#include "display_driver.h"
#include "board_config.h"
#include "jpeg_decoder.h"
#include "banner.h"

static esp_err_t draw_row(void *context, int x, int y, uint16_t *pixels, int width)
{
    (void)context;
    banner_apply_row(pixels, y, x, width);
    return display_driver_write(x, y, width, 1, pixels);
}

esp_err_t content_view_show(const content_item_t *item, const http_blob_t *jpeg)
{
    char title[CONTENT_TITLE_MAX + CONTENT_TIME_MAX + 1];
    content_item_banner(item, title, sizeof(title));
    ui_animation_prepare_image_mode();
    esp_err_t err = ESP_OK;
    if (jpeg->data) {
        banner_prepare(title);
        err = jpeg_decoder_render(jpeg->data, jpeg->size, DISPLAY_WIDTH, DISPLAY_HEIGHT, draw_row, NULL);
        if (err == ESP_OK) banner_begin_animation();
    }
    if (!jpeg->data || err != ESP_OK) banner_show_solid(0x203040, title);
    ui_animation_update_bottom_bar(0);
    return err;
}

void content_view_tick(void)
{
    banner_tick();
    ui_animation_task();
}
