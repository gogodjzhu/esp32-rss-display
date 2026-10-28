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
#include "cjk_font.h"

static const char *TAG = "UI";

/* ---------- ST7789 引脚定义（按芯片条件编译） ---------- */
/*
 * 硬件直连（IO_MUX）性能最佳，但 C3 与 S3 的原生引脚组不同：
 *   - ESP32-S3：SPI2 的 IO_MUX 原生引脚组为 GPIO10~14
 *     （已刻意避开 GPIO19/20 USB、GPIO0/3/45/46 strapping、
 *      GPIO26~32 内部 flash/PSRAM、GPIO33~37 Octal PSRAM）
 *   - ESP32-C3：SPI2(FSPI) 的原生 IO_MUX 直连为 GPIO6~11
 *     （FSPICLK=6 FSPID=7 FSPIHD=8 FSPIWP=9 FSPICS0=10 FSPIQ=11）
 * 若硬件接线不同，改对应分支即可（注意保持 DC/CS/RST 为普通可用 IO）。
 */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define TFT_MOSI  2    /* FSPID   */
#define TFT_SCLK  1    /* FSPICLK */
#define TFT_CS    6    /* FSPICS0 */
#define TFT_DC    5    /* FSPIQ   */
#define TFT_RST   4    /* FSPIWP  */
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
/* 沿用原 C3 接线的 GPIO 直连矩阵软连方案（SPI2_HOST + IO_MUX 均可） */
#define TFT_MOSI  1
#define TFT_SCLK  0
#define TFT_CS    19
#define TFT_DC    18
#define TFT_RST   3
#else
#error "不支持的芯片目标：未定义 TFT 引脚布局（当前仅支持 ESP32C3 / ESP32S3）"
#endif

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
static lv_obj_t *s_spinner    = NULL;
static lv_obj_t *s_bar        = NULL;
static lv_obj_t *s_bottom_bar = NULL;  /* 底部进度条（图片模式专用） */

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
    s_spinner    = NULL;
    s_bar        = NULL;
    s_bottom_bar = NULL;

    /* 深色背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* ---------- 公开 API 实现 ---------- */

void ui_animation_init(void)
{
    tft_init();
    lvgl_init();

    /* 加载中文子集字体（从 font flash 分区 mmap），失败不致命，回退默认字体 */
    cjk_font_init();

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
    lv_obj_set_scrollable(circle, false);

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

/* ---------- 新增接口实现 ---------- */

void ui_animation_show_no_network(const char *ap_ssid)
{
    ui_reset_screen();

    lv_obj_t *scr = lv_screen_active();

    /* 黄色背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1400), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* 黄色圆形 */
    lv_obj_t *circle = lv_obj_create(scr);
    lv_obj_set_size(circle, 100, 100);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0xf39c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(circle, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(circle, false);

    /* 感叹号竖线：从 (50,20) → (50,58) */
    static lv_point_precise_t pts_line[2] = {{50, 20}, {50, 58}};
    lv_obj_t *line = lv_line_create(circle);
    lv_line_set_points(line, pts_line, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_line_width(line, 8, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
    lv_obj_set_pos(line, 0, 0);

    /* 感叹号圆点：小圆圈在底部 */
    lv_obj_t *dot = lv_obj_create(circle);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_pos(dot, 45, 68);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(dot, false);

    /* 文字说明：热点名称和配网地址 */
    lv_obj_t *label1 = lv_label_create(scr);
    lv_label_set_text(label1, "Connect to WiFi hotspot:");
    lv_obj_set_style_text_color(label1, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(label1, LV_ALIGN_CENTER, 0, 55);

    lv_obj_t *label2 = lv_label_create(scr);
    lv_label_set_text_fmt(label2, "SSID: %s", ap_ssid);
    lv_obj_set_style_text_color(label2, lv_color_hex(0xf39c12), LV_PART_MAIN);
    lv_obj_align(label2, LV_ALIGN_CENTER, 0, 80);

    lv_obj_t *label3 = lv_label_create(scr);
    lv_label_set_text(label3, "Open: 192.168.4.1");
    lv_obj_set_style_text_color(label3, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label3, LV_ALIGN_CENTER, 0, 105);
}

/* 在连接过渡画面上添加一行「标签 + 数值」信息（左灰标签、右白数值） */
static void connecting_add_row(lv_obj_t *scr, int y, const char *label, const char *value)
{
    lv_obj_t *lb = lv_label_create(scr);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x8a8a9a), LV_PART_MAIN);
    lv_obj_set_pos(lb, 14, y);

    lv_obj_t *val = lv_label_create(scr);
    lv_label_set_text(val, (value && value[0]) ? value : "-");
    lv_obj_set_style_text_color(val, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(val, 234);
    lv_obj_set_pos(val, 72, y);
}

void ui_animation_show_connecting(const ui_connecting_info_t *info)
{
    ui_reset_screen();

    lv_obj_t *scr = lv_screen_active();

    /* 黑色背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* 顶部：spinner 与提示文字水平并排，节省纵向空间给信息列表 */
    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_set_pos(spinner, 14, 12);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Connecting to server...");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_pos(title, 66, 25);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 292, 1);
    lv_obj_set_pos(line, 14, 64);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x333344), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(line, false);

    /* 信息列表：固件版本 / 当前 SSID / 设备 IP / 后端地址 */
    connecting_add_row(scr, 74,  "FW",   (info ? info->firmware    : NULL));
    connecting_add_row(scr, 104, "SSID", (info ? info->ssid        : NULL));
    connecting_add_row(scr, 134, "IP",   (info ? info->ip          : NULL));
    connecting_add_row(scr, 164, "URL",  (info ? info->backend_url : NULL));

    /* 驱动 LVGL 渲染一帧，确保画面更新 */
    ui_animation_task();
}

void ui_animation_update_bottom_bar(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    lv_obj_t *scr = lv_screen_active();

    /* 首次调用时创建底部进度条 */
    if (s_bottom_bar == NULL) {
        s_bottom_bar = lv_bar_create(scr);
        lv_obj_set_size(s_bottom_bar, TFT_WIDTH, 2);
        /* 固定在屏幕底部（y = TFT_HEIGHT - 2 = 238） */
        lv_obj_set_pos(s_bottom_bar, 0, TFT_HEIGHT - 2);
        lv_obj_set_style_bg_color(s_bottom_bar, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bottom_bar, lv_color_hex(0x3498db), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bottom_bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bottom_bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(s_bottom_bar, 0, LV_PART_MAIN);
        lv_bar_set_range(s_bottom_bar, 0, 100);
    }

    lv_bar_set_value(s_bottom_bar, percent, LV_ANIM_OFF);

    /* 驱动 LVGL tick 并刷新 */
    uint32_t now_ms  = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed = now_ms - s_last_tick_ms;
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
        s_last_tick_ms = now_ms;
    }
    lv_timer_handler();
}

void ui_animation_write_row(int x, int y, const uint16_t *rgb565, int width)
{
    /* 直接写入 TFT，绕过 LVGL，用于图片区域（y < TFT_HEIGHT - 20） */
    tft_set_window((uint16_t)x, (uint16_t)y, (uint16_t)(x + width - 1), (uint16_t)y);
    gpio_set_level(TFT_DC, 1);
    size_t bytes = (size_t)width * 2;
    uint8_t *ptr = (uint8_t *)rgb565;
    size_t remaining = bytes;
    while (remaining > 0) {
        size_t chunk = remaining > SPI_MAX_TRANSFER_BYTES ? SPI_MAX_TRANSFER_BYTES : remaining;
        spi_transaction_t t = {
            .length    = chunk * 8,
            .tx_buffer = ptr,
        };
        spi_device_polling_transmit(s_spi, &t);
        ptr       += chunk;
        remaining -= chunk;
    }
}

void ui_animation_prepare_image_mode(void)
{
    /* 清空 LVGL 屏幕（删除 spinner、bar 等所有组件），背景设为纯黑 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    s_spinner    = NULL;
    s_bar        = NULL;
    s_bottom_bar = NULL;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* 预创建底部进度条，固定在 y=238，高 2px
     * 后续 lv_timer_handler 只会重绘这条进度条，不会覆盖图片区域 */
    s_bottom_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bottom_bar, TFT_WIDTH, 2);
    lv_obj_set_pos(s_bottom_bar, 0, TFT_HEIGHT - 2);
    lv_obj_set_style_bg_color(s_bottom_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bottom_bar, lv_color_hex(0x3498db), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bottom_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bottom_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_bottom_bar, 0, LV_PART_MAIN);
    lv_bar_set_range(s_bottom_bar, 0, 100);
    lv_bar_set_value(s_bottom_bar, 0, LV_ANIM_OFF);

    /* 立即刷新一次，让 LVGL 把纯黑背景 + 空进度条写入屏幕 */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    lv_tick_inc(now_ms - s_last_tick_ms);
    s_last_tick_ms = now_ms;
    lv_timer_handler();
}

/* ---------- 评价指示条（直接写 TFT，复用底部 2px 区域） ---------- */

/**
 * @brief 显示评分指示条
 *
 * sel = 0：无评价，全暗（5段均为暗灰）
 * sel = 1-5：对应评分，前 sel 段点亮，颜色从红渐变到绿
 *   1分=红(0xE74C3C)  2分=橙(0xE67E22)  3分=黄(0xF1C40F)
 *   4分=浅绿(0x2ECC71) 5分=绿(0x27AE60)
 * 每段宽 64px（320 / 5），底部 2px 行（y:238, y:239）
 */
void ui_animation_show_rating_bar(int sel)
{
    /* 将 24bit RGB 转为 RGB565 大端序（字节交换） */
    #define RGB24_TO_565_SWAPPED(c) ({ \
        uint8_t _r = ((c) >> 16) & 0xFF; \
        uint8_t _g = ((c) >>  8) & 0xFF; \
        uint8_t _b = ((c)      ) & 0xFF; \
        uint16_t _v = ((_r & 0xF8) << 8) | ((_g & 0xFC) << 3) | (_b >> 3); \
        (uint16_t)((_v >> 8) | (_v << 8)); \
    })

    /* 6 个状态的颜色（0=暗，1-5 从红到绿） */
    static const uint32_t SCORE_COLORS[6] = {
        0x333333,   /* 0: 暗灰（无评价） */
        0xE74C3C,   /* 1: 红 */
        0xE67E22,   /* 2: 橙 */
        0xF1C40F,   /* 3: 黄 */
        0x2ECC71,   /* 4: 浅绿 */
        0x27AE60,   /* 5: 绿 */
    };

    /* 每段宽度（5段均分 320px） */
    static const int SEG_W = TFT_WIDTH / 5;  /* 64px */

    uint16_t dim_color = RGB24_TO_565_SWAPPED(0x333333);

    /* 构建 320px 宽的行缓冲区 */
    uint16_t row[TFT_WIDTH];
    for (int seg = 0; seg < 5; seg++) {
        /* sel > 0 且当前段在评分范围内（seg < sel），则点亮；否则暗灰 */
        uint16_t color;
        if (sel > 0 && seg < sel) {
            color = RGB24_TO_565_SWAPPED(SCORE_COLORS[seg + 1]);
        } else {
            color = dim_color;
        }
        int x_start = seg * SEG_W;
        int x_end   = (seg == 4) ? TFT_WIDTH : x_start + SEG_W;  /* 最后一段补齐余数 */
        for (int x = x_start; x < x_end; x++) row[x] = color;
    }

    /* 直接写 TFT 底部两行（y:238, y:239） */
    ui_animation_write_row(0, TFT_HEIGHT - 2, row, TFT_WIDTH);
    ui_animation_write_row(0, TFT_HEIGHT - 1, row, TFT_WIDTH);

    #undef RGB24_TO_565_SWAPPED
}

/* ---------- 评分提交结果状态栏（直接写 TFT，复用底部 2px 区域） ---------- */

/**
 * @brief 显示评分提交结果状态栏
 *
 * 全行填充单一颜色覆盖底部 2px：
 *   success=true  → 全绿 0x2ECC71
 *   success=false → 全红 0xE74C3C
 */
void ui_animation_show_submit_bar(bool success)
{
    /* 将 24bit RGB 转为 RGB565 大端序（字节交换） */
    #define RGB24_TO_565_SWAPPED(c) ({ \
        uint8_t _r = ((c) >> 16) & 0xFF; \
        uint8_t _g = ((c) >>  8) & 0xFF; \
        uint8_t _b = ((c)      ) & 0xFF; \
        uint16_t _v = ((_r & 0xF8) << 8) | ((_g & 0xFC) << 3) | (_b >> 3); \
        (uint16_t)((_v >> 8) | (_v << 8)); \
    })

    uint16_t color = success
        ? RGB24_TO_565_SWAPPED(0x2ECC71)   /* 绿：成功 */
        : RGB24_TO_565_SWAPPED(0xE74C3C);  /* 红：失败 */

    uint16_t row[TFT_WIDTH];
    for (int x = 0; x < TFT_WIDTH; x++) row[x] = color;

    ui_animation_write_row(0, TFT_HEIGHT - 2, row, TFT_WIDTH);
    ui_animation_write_row(0, TFT_HEIGHT - 1, row, TFT_WIDTH);

    #undef RGB24_TO_565_SWAPPED
}

/* ======================================================================
 *                       中文点阵字体测试画面
 * ==================================================================== */

/**
 * @brief 选择中文字体；不可用时回退 LVGL 默认字体
 */
static const lv_font_t *pick_cjk_font(int size_px)
{
    const lv_font_t *font = cjk_font_get(size_px);
    return font ? font : LV_FONT_DEFAULT;
}

void ui_animation_show_font_test(void)
{
    ui_reset_screen();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* 标题（24px） */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "中文显示测试");
    lv_obj_set_style_text_font(title, pick_cjk_font(24), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_pos(title, 6, 4);

    /* 副标题：数字与中英混排（16px） */
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "0123456789 ABC abc ！？，。、“”（）《》");
    lv_obj_set_style_text_font(sub, pick_cjk_font(16), LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9be7ff), LV_PART_MAIN);
    lv_obj_set_pos(sub, 6, 34);

    /* 正文一：《千字文》（16px，自动换行，约 3 行） */
    lv_obj_t *body1 = lv_label_create(scr);
    lv_label_set_text(body1,
        "天地玄黄，宇宙洪荒。日月盈昃，辰宿列张。寒来暑往，秋收冬藏。"
        "闰余成岁，律吕调阳。云腾致雨，露结为霜。");
    lv_obj_set_style_text_font(body1, pick_cjk_font(16), LV_PART_MAIN);
    lv_obj_set_style_text_color(body1, lv_color_hex(0xe8e8e8), LV_PART_MAIN);
    lv_label_set_long_mode(body1, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body1, 308);
    lv_obj_set_pos(body1, 6, 58);

    /* 正文二：《兰亭集序》+《百家姓》（16px，约 3 行） */
    lv_obj_t *body2 = lv_label_create(scr);
    lv_label_set_text(body2,
        "永和九年，岁在癸丑，暮春之初，会于会稽山阴之兰亭。"
        "赵钱孙李，周吴郑王，冯陈褚卫，蒋沈韩杨。");
    lv_obj_set_style_text_font(body2, pick_cjk_font(16), LV_PART_MAIN);
    lv_obj_set_style_text_color(body2, lv_color_hex(0xe8e8e8), LV_PART_MAIN);
    lv_label_set_long_mode(body2, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body2, 308);
    lv_obj_set_pos(body2, 6, 142);

    /* 立即渲染一帧 */
    ui_animation_task();
}

/* ======================================================================
 *             图片标题浮层（BBC 风格上下滚动 banner + 中文）
 * ==================================================================== */

/* banner 几何：窗口高度为「一行 + 上下内边距」，同一时刻只显示一行；
 * 底部保留 2px 进度条 / 评价条。 */
#define BANNER_FONT_PX     18
#define BANNER_PAD_X       6
#define BANNER_PAD_Y       13         /* 行上下内边距：越大 banner 越高 */
#define BANNER_MAX_LINES   12
#define BANNER_MAX_GLYPHS  160
#define BANNER_MAX_H       72          /* 背景缓冲最大高度（像素），需 >= 行高 + 2*BANNER_PAD_Y */
#define BANNER_BG_ALPHA    150         /* 0-255，banner 底色不透明度 */
#define BANNER_HOLD_MS     2500        /* 每行停留时间（毫秒） */
#define BANNER_SCROLL_MS   450         /* 行切换滚动时间（毫秒） */

typedef struct {
    int16_t             x;       /* 屏幕 x（含左内边距） */
    int16_t             y_off;   /* 相对所在行顶部的 y 偏移 */
    uint16_t            w, h;    /* 字形位图尺寸 */
    uint16_t            stride;  /* A8 位图行跨距 */
    const uint8_t      *bmp;     /* A8 位图 */
    uint8_t             line;    /* 所属行索引 */
    lv_font_glyph_dsc_t dsc;     /* 用于释放字形缓存 */
} banner_glyph_t;

static banner_glyph_t  s_banner_glyphs[BANNER_MAX_GLYPHS];
static int             s_banner_glyph_count = 0;
static int             s_banner_line_count  = 0;
static bool            s_banner_active      = false;   /* 有标题时才绘制 banner */
static bool            s_banner_capture     = false;   /* 首帧解码图片时捕获背景 */

/* 运行时几何（依赖字体行高） */
static int             s_banner_line_h = 0;
static int             s_banner_h      = 0;
static int             s_banner_y0     = 0;
static int             s_banner_y1     = 0;

/* banner 窗口背景像素（已叠加半透明底色），供滚动重绘复用 */
static uint16_t        s_bg_buf[TFT_WIDTH * BANNER_MAX_H];
static bool            s_bg_valid      = false;

/* 滚动动画状态 */
static int             s_cur_line        = 0;   /* 当前停留行 */
static int             s_scroll          = 0;   /* 0..line_h，当前滚动偏移 */
static bool            s_scrolling       = false;
static int64_t         s_scroll_start_us = 0;
static int64_t         s_hold_start_us   = 0;

/* 将 uint16 RGB565 在字节序与大端之间互换（与 ui_animation_write_row 的约定一致） */
static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

/* RGB565 前景/背景混合，a 为 0-255 不透明度 */
static inline uint16_t blend565(uint16_t bg, uint8_t fr, uint8_t fg, uint8_t fb, uint8_t a)
{
    int br = (bg >> 11) & 0x1F;
    int bgc = (bg >> 5) & 0x3F;
    int bb = bg & 0x1F;
    int fr5 = fr >> 3, fg6 = fg >> 2, fb5 = fb >> 3;

    int nr = br  + (((fr5 - br)  * a + 127) / 255);
    int ng = bgc + (((fg6 - bgc) * a + 127) / 255);
    int nb = bb  + (((fb5 - bb)  * a + 127) / 255);

    if (nr < 0) nr = 0; else if (nr > 31) nr = 31;
    if (ng < 0) ng = 0; else if (ng > 63) ng = 63;
    if (nb < 0) nb = 0; else if (nb > 31) nb = 31;

    return (uint16_t)((nr << 11) | (ng << 5) | nb);
}

/* 解码一个 UTF-8 码点；非法序列按单字节处理。pp 会前进 */
static uint32_t utf8_next(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t cp = *p;

    if (cp < 0x80) {
        *pp = (const char *)(p + 1);
        return cp;
    } else if ((cp & 0xE0) == 0xC0 && p[1]) {
        cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
        *pp = (const char *)(p + 2);
    } else if ((cp & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *pp = (const char *)(p + 3);
    } else if ((cp & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((cp & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *pp = (const char *)(p + 4);
    } else {
        *pp = (const char *)(p + 1);
    }
    return cp;
}

/* 是否为可断行的空白字符 */
static bool cp_is_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t';
}

/* 是否为 CJK 字符：可在任意两字之间断行（无需空格） */
static bool cp_is_cjk(uint32_t cp)
{
    return (cp >= 0x1100  && cp <= 0x11FF) ||   /* 谚文字母 */
           (cp >= 0x2E80  && cp <= 0x303F) ||   /* CJK 部首扩展 / 标点 */
           (cp >= 0x3040  && cp <= 0x30FF) ||   /* 日文假名 */
           (cp >= 0x3130  && cp <= 0x318F) ||   /* 谚文兼容字母 */
           (cp >= 0x3400  && cp <= 0x4DBF) ||   /* CJK 扩展 A */
           (cp >= 0x4E00  && cp <= 0x9FFF) ||   /* CJK 基本区 */
           (cp >= 0xAC00  && cp <= 0xD7FF) ||   /* 谚文音节 */
           (cp >= 0xF900  && cp <= 0xFAFF) ||   /* CJK 兼容表意 */
           (cp >= 0xFE30  && cp <= 0xFE4F) ||   /* CJK 兼容符号 */
           (cp >= 0xFF00  && cp <= 0xFFEF) ||   /* 全角字符 */
           (cp >= 0x20000 && cp <= 0x2FA1F);    /* CJK 扩展 B~F */
}

void ui_animation_banner_release(void)
{
    for (int i = 0; i < s_banner_glyph_count; i++) {
        lv_font_glyph_release_draw_data(&s_banner_glyphs[i].dsc);
    }
    s_banner_glyph_count = 0;
    s_banner_line_count  = 0;
    s_banner_active      = false;
    s_banner_capture     = false;
    s_bg_valid           = false;
    s_scrolling          = false;
}

void ui_animation_banner_prepare(const char *title)
{
    ui_animation_banner_release();

    if (title == NULL || title[0] == '\0') {
        return;
    }

    const lv_font_t *font = cjk_font_get(BANNER_FONT_PX);
    if (font == NULL) {
        return;
    }

    const int line_h = font->line_height;
    if (line_h <= 0 || line_h > BANNER_MAX_H) {
        ESP_LOGW(TAG, "字体行高 %d 异常，标题浮层不可用", line_h);
        return;
    }

    /* banner 窗口几何：一行高 + 上下内边距，底部留 2px 进度条 */
    s_banner_line_h = line_h;
    s_banner_h      = line_h + 2 * BANNER_PAD_Y;
    if (s_banner_h > BANNER_MAX_H) s_banner_h = BANNER_MAX_H;
    s_banner_y1     = TFT_HEIGHT - 3;
    s_banner_y0     = s_banner_y1 - s_banner_h + 1;
    if (s_banner_y0 < 0) s_banner_y0 = 0;

    const int x_left = BANNER_PAD_X;
    const int max_w  = TFT_WIDTH - 2 * BANNER_PAD_X;

    /* 第一遍：按常规文本规则换行（拉丁词整体不拆分，CJK 可逐字断行），
     * 并记录每个字的笔位置与所属行 */
    typedef struct { int16_t px; uint8_t line; const char *cp; } placed_t;
    static placed_t placed[BANNER_MAX_GLYPHS];
    int placed_cnt = 0;
    int pen_x = x_left;
    int line  = 0;
    int pending_space_w = 0;   /* 上一个词之后累积的空白宽度（换行时丢弃） */

    const char *p = title;
    while (*p && placed_cnt < BANNER_MAX_GLYPHS) {
        /* 显式换行 */
        {
            const char *q0 = p;
            uint32_t    c0 = utf8_next(&q0);
            if (c0 == '\n' || c0 == '\r') {
                p = q0;
                line++;
                pen_x = x_left;
                pending_space_w = 0;
                if (line >= BANNER_MAX_LINES) break;
                continue;
            }
            /* 空白：累积间距，留到下一个词前再决定是否换行 */
            if (cp_is_space(c0)) {
                pending_space_w += lv_font_get_glyph_width(font, c0, 0);
                p = q0;
                continue;
            }
        }

        /* 确定一个「断行单元」的范围：CJK 单字，或一段连续的非空白非 CJK 词 */
        const char *unit_start = p;
        const char *unit_end;
        {
            const char *q = p;
            uint32_t    c = utf8_next(&q);
            if (cp_is_cjk(c)) {
                unit_end = q;   /* 单个字 */
            } else {
                const char *s = p;
                while (*s) {
                    const char *t = s;
                    uint32_t    cc = utf8_next(&t);
                    if (cp_is_space(cc) || cp_is_cjk(cc) ||
                        cc == '\n' || cc == '\r') {
                        break;
                    }
                    s = t;
                }
                unit_end = s;
            }
        }

        /* 计算单元宽度 */
        int unit_w = 0;
        for (const char *s = unit_start; s < unit_end; ) {
            uint32_t c = utf8_next(&s);
            unit_w += lv_font_get_glyph_width(font, c, 0);
        }

        /* 放不下则整词换到下一行（行首丢弃空白） */
        int sep = (pen_x > x_left) ? pending_space_w : 0;
        if (pen_x > x_left && pen_x + sep + unit_w > x_left + max_w) {
            line++;
            pen_x = x_left;
            sep = 0;
            if (line >= BANNER_MAX_LINES) break;
        }
        if (sep > 0) {
            pen_x += sep;   /* 空白只占位，不绘制 */
        }
        pending_space_w = 0;

        /* 放置单元内每个字；超长单词退化为逐字断行 */
        for (const char *s = unit_start; s < unit_end && placed_cnt < BANNER_MAX_GLYPHS; ) {
            const char *t = s;
            uint32_t    c = utf8_next(&t);
            int adv = lv_font_get_glyph_width(font, c, 0);

            if (pen_x > x_left && pen_x + adv > x_left + max_w) {
                line++;
                pen_x = x_left;
                if (line >= BANNER_MAX_LINES) break;
            }
            placed[placed_cnt].px   = (int16_t)pen_x;
            placed[placed_cnt].line = (uint8_t)line;
            placed[placed_cnt].cp   = s;
            placed_cnt++;

            pen_x += adv;
            s = t;
        }

        p = unit_end;
    }

    int used_lines = (placed_cnt > 0) ? (placed[placed_cnt - 1].line + 1) : 0;
    if (used_lines <= 0) {
        return;
    }

    /* 第二遍：取字形位图，记录窗口内相对坐标（行内偏移 + 行索引） */
    for (int i = 0; i < placed_cnt; i++) {
        const char *q  = placed[i].cp;
        uint32_t    cp = utf8_next(&q);
        uint32_t    next = 0;
        if (*q) { const char *q2 = q; next = utf8_next(&q2); }

        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(font, &dsc, cp, next)) {
            continue;
        }
        if (dsc.box_w == 0 || dsc.box_h == 0) {
            continue;   /* 空格等无位图字形 */
        }
        const void *bmp = lv_font_get_glyph_bitmap(&dsc, NULL);
        if (bmp == NULL) {
            lv_font_glyph_release_draw_data(&dsc);
            continue;
        }
        const lv_draw_buf_t *db = (const lv_draw_buf_t *)bmp;

        banner_glyph_t *g = &s_banner_glyphs[s_banner_glyph_count];
        g->x      = (int16_t)(placed[i].px + dsc.ofs_x);
        g->y_off  = (int16_t)((font->line_height - font->base_line) - dsc.box_h - dsc.ofs_y);
        g->w      = dsc.box_w;
        g->h      = dsc.box_h;
        g->stride = (uint16_t)db->header.stride;
        g->bmp    = db->data;
        g->line   = placed[i].line;
        g->dsc    = dsc;
        s_banner_glyph_count++;
    }

    if (s_banner_glyph_count == 0) {
        return;
    }

    s_banner_line_count = used_lines;
    s_banner_active     = true;
    s_banner_capture    = true;    /* 首帧解码图片时捕获背景 */
    s_cur_line          = 0;
    s_scroll            = 0;
    s_scrolling         = false;

    /* 预填黑色，避免图片未覆盖区域出现脏数据 */
    memset(s_bg_buf, 0, sizeof(s_bg_buf));

    ESP_LOGI(TAG, "标题浮层排版：%d 字形，%d 行，窗口 y=%d..%d（行高 %d）",
             s_banner_glyph_count, s_banner_line_count,
             s_banner_y0, s_banner_y1, s_banner_line_h);
}

/* 将当前行（滚动时叠加下一行）的标题文字混合到一行缓冲区 */
static void banner_draw_glyphs(uint16_t *row, int screen_y, int x_start, int width)
{
    if (!s_banner_active || s_banner_line_h <= 0) return;

    const int base_cur = s_banner_y0 + BANNER_PAD_Y - s_scroll;
    const int base_nxt = base_cur + s_banner_line_h;
    const int cur = s_cur_line;
    const int nxt = s_scrolling ? ((s_cur_line + 1) % s_banner_line_count) : -1;

    for (int gi = 0; gi < s_banner_glyph_count; gi++) {
        const banner_glyph_t *g = &s_banner_glyphs[gi];
        int base;
        if ((int)g->line == cur) {
            base = base_cur;
        } else if (nxt >= 0 && (int)g->line == nxt) {
            base = base_nxt;
        } else {
            continue;
        }

        int ly = screen_y - (base + g->y_off);
        if (ly < 0 || ly >= g->h) continue;

        const uint8_t *line = g->bmp + (size_t)ly * g->stride;
        for (int col = 0; col < g->w; col++) {
            int x = g->x + col;
            if (x < x_start || x >= x_start + width) continue;
            uint8_t a = line[col];
            if (a == 0) continue;
            uint16_t real = swap16(row[x - x_start]);
            real = blend565(real, 0xFF, 0xFF, 0xFF, a);
            row[x - x_start] = swap16(real);
        }
    }
}

void ui_animation_banner_apply_row(uint16_t *row, int screen_y, int x_start, int width)
{
    if (row == NULL || width <= 0 || screen_y < 0 || screen_y >= TFT_HEIGHT) {
        return;
    }

    /* 1) 半透明深色 banner 背景（仅在确有标题时绘制），
     *    同时把结果缓存为后续滚动重绘的背景底图 */
    if (s_banner_active && screen_y >= s_banner_y0 && screen_y <= s_banner_y1) {
        int by = screen_y - s_banner_y0;
        for (int i = 0; i < width; i++) {
            int x = x_start + i;
            if (x < 0 || x >= TFT_WIDTH) continue;
            uint16_t real = swap16(row[i]);
            real = blend565(real, 0, 0, 0, BANNER_BG_ALPHA);
            row[i] = swap16(real);
            if (s_banner_capture) {
                s_bg_buf[by * TFT_WIDTH + x] = row[i];
            }
        }
    }

    /* 2) 白色标题文字（按字形 alpha 混合） */
    banner_draw_glyphs(row, screen_y, x_start, width);
}

/* 用缓存的背景底图 + 当前滚动偏移重绘整个 banner 窗口 */
static void banner_redraw(void)
{
    static uint16_t row[TFT_WIDTH];
    for (int y = s_banner_y0; y <= s_banner_y1; y++) {
        int by = y - s_banner_y0;
        memcpy(row, &s_bg_buf[by * TFT_WIDTH], sizeof(uint16_t) * TFT_WIDTH);
        banner_draw_glyphs(row, y, 0, TFT_WIDTH);
        ui_animation_write_row(0, y, row, TFT_WIDTH);
    }
}

void ui_animation_banner_begin_animation(void)
{
    s_banner_capture  = false;
    s_bg_valid        = s_banner_active;
    s_cur_line        = 0;
    s_scroll          = 0;
    s_scrolling       = false;
    s_hold_start_us   = esp_timer_get_time();
    s_scroll_start_us = s_hold_start_us;
}

void ui_animation_banner_tick(void)
{
    if (!s_banner_active || !s_bg_valid || s_banner_line_count <= 1) {
        return;
    }

    const int64_t now = esp_timer_get_time();

    if (s_scrolling) {
        /* 滚动中：按已过时间线性推进整行位移 */
        int64_t el = now - s_scroll_start_us;
        if (el >= (int64_t)BANNER_SCROLL_MS * 1000) {
            s_cur_line      = (s_cur_line + 1) % s_banner_line_count;
            s_scroll        = 0;
            s_scrolling     = false;
            s_hold_start_us = now;
        } else {
            s_scroll = (int)((int64_t)s_banner_line_h * el /
                             ((int64_t)BANNER_SCROLL_MS * 1000));
        }
        banner_redraw();
    } else if (now - s_hold_start_us >= (int64_t)BANNER_HOLD_MS * 1000) {
        /* 停留结束：开始滚动到下一行 */
        s_scrolling       = true;
        s_scroll_start_us = now;
        s_scroll          = 0;
    }
}

void ui_animation_show_solid_banner(uint32_t bg_rgb24, const char *title)
{
    ui_animation_banner_prepare(title);

    uint8_t  r = (bg_rgb24 >> 16) & 0xFF;
    uint8_t  g = (bg_rgb24 >>  8) & 0xFF;
    uint8_t  b = (bg_rgb24      ) & 0xFF;
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    uint16_t base = swap16(v);

    static uint16_t row[TFT_WIDTH];
    for (int y = 0; y < TFT_HEIGHT; y++) {
        for (int x = 0; x < TFT_WIDTH; x++) row[x] = base;
        ui_animation_banner_apply_row(row, y, 0, TFT_WIDTH);
        ui_animation_write_row(0, y, row, TFT_WIDTH);
    }

    /* 保留排版与背景底图，交由主循环 tick 驱动滚动动画 */
    ui_animation_banner_begin_animation();
}
