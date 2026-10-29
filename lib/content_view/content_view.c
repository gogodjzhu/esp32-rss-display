#include "content_view.h"
#include "ui_animation.h"
#include "display_driver.h"
#include "board_config.h"
#include "jpeg_decoder.h"
#include "banner.h"

/* 转换时按 CONTENT_STOCK_MAX 声明数组并交给 banner；上限必须保持一致 */
_Static_assert(CONTENT_STOCK_MAX == BANNER_STOCK_MAX, "行情数量上限不一致");

static esp_err_t draw_row(void *context, int x, int y, uint16_t *pixels, int width)
{
    (void)context;
    banner_apply_row(pixels, y, x, width);
    return display_driver_write(x, y, width, 1, pixels);
}

/* 把内容协议转换为 banner 排版模型；名称保留原文，显示时由 banner 按像素宽度裁剪。
 * 转换结果只在 banner_prepare() 期间使用，不保留 item 内部指针。 */
static void build_banner(const content_item_t *item, banner_content_t *content,
                         banner_stock_t stocks[CONTENT_STOCK_MAX])
{
    uint8_t count = item->stock_count > CONTENT_STOCK_MAX ? CONTENT_STOCK_MAX : item->stock_count;
    for (uint8_t i = 0; i < count; i++) {
        stocks[i].name      = item->stocks[i].name;
        stocks[i].change_bp = item->stocks[i].change_bp;
    }

    *content = (banner_content_t){
        .time        = item->published_at,
        .title       = item->title,
        .stocks      = stocks,
        .stock_count = count,
    };
}

esp_err_t content_view_show(const content_item_t *item, const http_blob_t *jpeg)
{
    banner_content_t content;
    banner_stock_t stocks[CONTENT_STOCK_MAX];
    build_banner(item, &content, stocks);

    ui_animation_prepare_image_mode();
    esp_err_t err = ESP_OK;
    if (jpeg->data) {
        banner_prepare(&content);
        err = jpeg_decoder_render(jpeg->data, jpeg->size, DISPLAY_WIDTH, DISPLAY_HEIGHT, draw_row, NULL);
        if (err == ESP_OK) banner_begin_animation();
    }
    if (!jpeg->data || err != ESP_OK) banner_show_solid(0x203040, &content);
    ui_animation_update_progress(0);
    return err;
}

void content_view_tick(void)
{
    banner_tick();
    ui_animation_task();
}
