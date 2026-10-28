/**
 * @file ui_animation.c
 * @brief LVGL 页面与底栏；屏幕传输由 display_driver 负责
 */

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "lv_conf.h"
#include "lvgl.h"
#include "ui_animation.h"
#include "cjk_font.h"
#include "display_driver.h"
#include "board_config.h"
#include "banner.h"

static const char *TAG = "UI";
#define TFT_WIDTH DISPLAY_WIDTH
#define TFT_HEIGHT DISPLAY_HEIGHT
#define CHUNK_HEIGHT 20
#define BUF_SIZE (TFT_WIDTH * CHUNK_HEIGHT * 2)
static lv_display_t *s_disp;
static uint8_t s_buf1[BUF_SIZE];
static uint8_t s_buf2[BUF_SIZE];
static lv_obj_t *s_spinner;

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    ESP_ERROR_CHECK(display_driver_write(area->x1, area->y1,
        area->x2 - area->x1 + 1, area->y2 - area->y1 + 1, (const uint16_t *)pixels));
    lv_display_flush_ready(display);
}

/* ---------- LVGL 初始化 ---------- */

static uint32_t ui_clock_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_init(void)
{
    lv_init();
    lv_tick_set_cb(ui_clock_ms);

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
    banner_release();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    s_spinner = NULL;
    /* 深色背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* ---------- 公开 API 实现 ---------- */

void ui_animation_init(void)
{
    ESP_ERROR_CHECK(display_driver_init());
    lvgl_init();

    /* 加载中文子集字体（从 font flash 分区 mmap），失败不致命，回退默认字体 */
    cjk_font_init();

    /* 初始显示空白深色背景 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);

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

void ui_animation_show_error(void)
{
    ui_reset_screen();

    lv_obj_t *scr = lv_screen_active();

    /* 红色背景提示错误 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x2d0a0a), LV_PART_MAIN);
}

void ui_animation_task(void)
{
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

/* 底部 2px 由进度条、评分条和结果条共用；记录当前内容，避免重复写同一条 */
static int s_bar_percent = -1;

static void write_row(int x, int y, const uint16_t *pixels, int width)
{
    ESP_ERROR_CHECK(display_driver_write(x, y, width, 1, pixels));
}

static void draw_bottom_bar(const uint16_t *row)
{
    write_row(0, TFT_HEIGHT - 2, row, TFT_WIDTH);
    write_row(0, TFT_HEIGHT - 1, row, TFT_WIDTH);
}

void ui_animation_invalidate_bottom_bar(void)
{
    s_bar_percent = -1;
}

void ui_animation_update_bottom_bar(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* 画面上别无变化时跳过 SPI 写入，只有内容变化或被覆盖后才重绘 */
    if (percent == s_bar_percent) return;
    uint16_t row[TFT_WIDTH];
    for (int x = 0; x < TFT_WIDTH; x++) row[x] = x < TFT_WIDTH * percent / 100 ? 0x7634 : 0x8631;
    draw_bottom_bar(row);
    s_bar_percent = percent;
}

void ui_animation_prepare_image_mode(void)
{
    ui_reset_screen();
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), LV_PART_MAIN);
    /* 先完成 LVGL 的整屏刷新，此后图片、标题和底栏由同一任务直接绘制 */
    lv_refr_now(s_disp);
    ui_animation_invalidate_bottom_bar();
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

    /* 直接写 TFT 底部两行（y:238, y:239），进度条状态随之失效 */
    draw_bottom_bar(row);
    ui_animation_invalidate_bottom_bar();

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

    draw_bottom_bar(row);
    ui_animation_invalidate_bottom_bar();

    #undef RGB24_TO_565_SWAPPED
}
