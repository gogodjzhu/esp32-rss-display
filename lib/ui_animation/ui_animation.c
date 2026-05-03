/**
 * @file ui_animation.c
 * @brief ST7789 SPI 驱动 + LVGL v9 初始化 + UI 状态动画
 *
 * 已验证参数（验证项目 /tmp/opencode/lvgl_verify）：
 *   - MADCTL = 0x60（MX|MV 横屏，无 BGR bit）
 *   - 无 INVON（不发 0x21，避免颜色反转）
 *   - row_offset = 0，col_offset = 0
 *   - LV_COLOR_FORMAT_RGB565_SWAPPED 解决 SPI 字节序
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <string.h>

#include "lv_conf.h"
#include "lvgl.h"
#include "ui_animation.h"

static const char *TAG = "UI";

/* ---------- ST7789 引脚定义 ---------- */
#define TFT_MOSI  6
#define TFT_SCLK  5
#define TFT_CS    7
#define TFT_DC    3
#define TFT_RST   10

#define TFT_WIDTH  320
#define TFT_HEIGHT 240

/* 分块渲染：320×20行 = 12800 像素 = 25600 bytes */
#define CHUNK_HEIGHT 20
#define BUF_SIZE     (TFT_WIDTH * CHUNK_HEIGHT)

/* ESP-IDF SPI DMA 单次传输上限：4092 bytes */
#define SPI_MAX_TRANSFER_BYTES 4092

/* ---------- 内部状态 ---------- */
static spi_device_handle_t s_spi;
static lv_display_t       *s_disp;
static lv_color_t          s_buf1[BUF_SIZE];
static lv_color_t          s_buf2[BUF_SIZE];

/* 当前 UI 组件引用（切换状态时复用） */
static lv_obj_t *s_spinner  = NULL;
static lv_obj_t *s_bar      = NULL;

/* tick 时间基准 */
static uint32_t s_last_tick_ms = 0;

/* ---------- ST7789 底层 SPI 操作 ---------- */

static void tft_send_cmd(uint8_t cmd)
{
    gpio_set_level(TFT_DC, 0);
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(TFT_DC, 1);
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t data[4];

    /* 列地址设置 */
    tft_send_cmd(0x2A);
    data[0] = x1 >> 8; data[1] = x1 & 0xFF;
    data[2] = x2 >> 8; data[3] = x2 & 0xFF;
    tft_send_data(data, 4);

    /* 行地址设置 */
    tft_send_cmd(0x2B);
    data[0] = y1 >> 8; data[1] = y1 & 0xFF;
    data[2] = y2 >> 8; data[3] = y2 & 0xFF;
    tft_send_data(data, 4);

    /* 开始写入内存 */
    tft_send_cmd(0x2C);
}

/* ---------- LVGL flush callback ---------- */

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w           = area->x2 - area->x1 + 1;
    uint32_t h           = area->y2 - area->y1 + 1;
    uint32_t total_bytes = w * h * 2;   /* RGB565：2 bytes/pixel */

    tft_set_window(area->x1, area->y1, area->x2, area->y2);

    /* 分段发送，每次不超过 SPI DMA 上限 */
    gpio_set_level(TFT_DC, 1);
    uint8_t  *ptr       = px_map;
    uint32_t  remaining = total_bytes;
    while (remaining > 0) {
        uint32_t chunk = remaining > SPI_MAX_TRANSFER_BYTES
                         ? SPI_MAX_TRANSFER_BYTES : remaining;
        spi_transaction_t t = {
            .length    = chunk * 8,
            .tx_buffer = ptr,
        };
        spi_device_polling_transmit(s_spi, &t);
        ptr       += chunk;
        remaining -= chunk;
    }

    lv_display_flush_ready(display);
}

/* ---------- ST7789 硬件初始化 ---------- */

static void tft_init(void)
{
    /* 配置 RST / DC 引脚为输出 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TFT_RST) | (1ULL << TFT_DC),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    /* 硬件复位 */
    gpio_set_level(TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* SPI 总线初始化 */
    spi_bus_config_t buscfg = {
        .mosi_io_num    = TFT_MOSI,
        .miso_io_num    = -1,
        .sclk_io_num    = TFT_SCLK,
        .quadwp_io_num  = -1,
        .quadhd_io_num  = -1,
        .max_transfer_sz = TFT_WIDTH * CHUNK_HEIGHT * 2 + 8,
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    /* ST7789 设备注册 */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  /* 40 MHz */
        .mode           = 0,
        .spics_io_num   = TFT_CS,
        .queue_size     = 7,
    };
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);

    /* ST7789 初始化序列 */
    tft_send_cmd(0x01);          /* 软件复位 */
    vTaskDelay(pdMS_TO_TICKS(150));
    tft_send_cmd(0x11);          /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* MADCTL：0x60 = MX|MV 横屏（320×240），不设 BGR bit */
    tft_send_cmd(0x36);
    uint8_t madctl = 0x60;
    tft_send_data(&madctl, 1);

    /* 像素格式：RGB565 */
    tft_send_cmd(0x3A);
    uint8_t pf = 0x55;
    tft_send_data(&pf, 1);

    /* 注：不发 INVON(0x21)，颜色由 LVGL RGB565_SWAPPED + MADCTL 控制 */

    tft_send_cmd(0x29);          /* Display On */
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "ST7789 初始化完成");
}

/* ---------- LVGL 初始化 ---------- */

static void lvgl_init(void)
{
    lv_init();

    s_disp = lv_display_create(TFT_WIDTH, TFT_HEIGHT);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, sizeof(s_buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* RGB565_SWAPPED 解决 SPI MSB/LSB 字节序 */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    ESP_LOGI(TAG, "LVGL v%d.%d.%d 初始化完成，显示 %dx%d",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             TFT_WIDTH, TFT_HEIGHT);
}

/* ---------- 内部辅助：清空屏幕并重置组件引用 ---------- */

static void ui_reset_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    s_spinner = NULL;
    s_bar     = NULL;

    /* 深色背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* ---------- 公开 API 实现 ---------- */

void ui_animation_init(void)
{
    tft_init();
    lvgl_init();
    s_last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* 初始显示空白深色背景 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xf295b9), LV_PART_MAIN);

    ESP_LOGI(TAG, "UI 初始化完成，空闲堆：%ld bytes", esp_get_free_heap_size());
}

void ui_animation_show_loading(void)
{
    ui_reset_screen();

    /* Spinner 居中 */
    s_spinner = lv_spinner_create(lv_screen_active());
    lv_obj_set_size(s_spinner, 60, 60);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, 0, 0);
}

void ui_animation_show_progress(int percent)
{
    ui_reset_screen();

    /* 进度条居中 */
    s_bar = lv_bar_create(lv_screen_active());
    lv_obj_set_size(s_bar, 240, 16);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 0);
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(s_bar, percent, LV_ANIM_ON);
}

void ui_animation_show_error(void)
{
    ui_reset_screen();

    lv_obj_t *scr = lv_screen_active();

    /* 红色背景提示错误 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x2d0a0a), LV_PART_MAIN);
}

void ui_animation_show_ok(void)
{
    ui_reset_screen();

    lv_obj_t *scr = lv_screen_active();

    /* 绿色实心圆 */
    lv_obj_t *circle = lv_obj_create(scr);
    lv_obj_set_size(circle, 100, 100);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0x27ae60), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(circle, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

    /* 用两段 lv_line 画白色对勾（坐标相对于圆心 50,50） */
    /* 短边：从 (22,50) → (40,68)，长边：从 (40,68) → (72,30) */
    static lv_point_precise_t pts_short[2] = {{22, 50}, {40, 68}};
    static lv_point_precise_t pts_long[2]  = {{40, 68}, {72, 30}};

    lv_obj_t *line1 = lv_line_create(circle);
    lv_line_set_points(line1, pts_short, 2);
    lv_obj_set_style_line_color(line1, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_line_width(line1, 6, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line1, true, LV_PART_MAIN);
    lv_obj_set_pos(line1, 0, 0);

    lv_obj_t *line2 = lv_line_create(circle);
    lv_line_set_points(line2, pts_long, 2);
    lv_obj_set_style_line_color(line2, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_line_width(line2, 6, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line2, true, LV_PART_MAIN);
    lv_obj_set_pos(line2, 0, 0);
}

void ui_animation_task(void)
{
    /* 手动驱动 LVGL tick（lv_conf.h 中已配置 LV_TICK_CUSTOM，此处双保险） */
    uint32_t now_ms  = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed = now_ms - s_last_tick_ms;
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
        s_last_tick_ms = now_ms;
    }

    lv_timer_handler();
}
