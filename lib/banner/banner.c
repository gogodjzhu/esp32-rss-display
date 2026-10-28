#include "banner.h"
#include "display_driver.h"
#include "board_config.h"
#include "cjk_font.h"
#include "lvgl.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#define TFT_WIDTH DISPLAY_WIDTH
#define TFT_HEIGHT DISPLAY_HEIGHT
static const char *TAG = "BANNER";

static void banner_write_row(int x, int y, const uint16_t *pixels, int width)
{
    ESP_ERROR_CHECK(display_driver_write(x, y, width, 1, pixels));
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
    lv_draw_buf_t      *owned;   /* 默认字体转换为 A8 后的自有缓冲 */
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

/* 将 uint16 RGB565 在字节序与大端之间互换（与 banner_write_row 的约定一致） */
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

void banner_release(void)
{
    for (int i = 0; i < s_banner_glyph_count; i++) {
        lv_font_glyph_release_draw_data(&s_banner_glyphs[i].dsc);
        if (s_banner_glyphs[i].owned) lv_draw_buf_destroy(s_banner_glyphs[i].owned);
    }
    s_banner_glyph_count = 0;
    s_banner_line_count  = 0;
    s_banner_active      = false;
    s_banner_capture     = false;
    s_bg_valid           = false;
    s_scrolling          = false;
}

void banner_prepare(const char *title)
{
    banner_release();

    if (title == NULL || title[0] == '\0') {
        return;
    }

    const lv_font_t *font = cjk_font_get(BANNER_FONT_PX);
    if (font == NULL) font = LV_FONT_DEFAULT;

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
            lv_font_glyph_release_draw_data(&dsc);
            continue;   /* 空格等无位图字形 */
        }
        /* 内置字体需要调用方提供解压目标；Tiny TTF 返回自身缓存 */
        lv_draw_buf_t *owned = NULL;
        if (dsc.resolved_font->get_glyph_bitmap == lv_font_get_bitmap_fmt_txt) {
            owned = lv_draw_buf_create(dsc.box_w, dsc.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
            if (!owned) { lv_font_glyph_release_draw_data(&dsc); continue; }
        }
        const void *bmp = lv_font_get_glyph_bitmap(&dsc, owned);
        if (bmp == NULL) {
            lv_font_glyph_release_draw_data(&dsc);
            if (owned) lv_draw_buf_destroy(owned);
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
        g->owned  = owned;
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

void banner_apply_row(uint16_t *row, int screen_y, int x_start, int width)
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
        banner_write_row(0, y, row, TFT_WIDTH);
    }
}

void banner_begin_animation(void)
{
    s_banner_capture  = false;
    s_bg_valid        = s_banner_active;
    s_cur_line        = 0;
    s_scroll          = 0;
    s_scrolling       = false;
    s_hold_start_us   = esp_timer_get_time();
    s_scroll_start_us = s_hold_start_us;
}

void banner_tick(void)
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

void banner_show_solid(uint32_t bg_rgb24, const char *title)
{
    banner_prepare(title);

    uint8_t  r = (bg_rgb24 >> 16) & 0xFF;
    uint8_t  g = (bg_rgb24 >>  8) & 0xFF;
    uint8_t  b = (bg_rgb24      ) & 0xFF;
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    uint16_t base = swap16(v);

    static uint16_t row[TFT_WIDTH];
    for (int y = 0; y < TFT_HEIGHT; y++) {
        for (int x = 0; x < TFT_WIDTH; x++) row[x] = base;
        banner_apply_row(row, y, 0, TFT_WIDTH);
        banner_write_row(0, y, row, TFT_WIDTH);
    }

    /* 保留排版与背景底图，交由主循环 tick 驱动滚动动画 */
    banner_begin_animation();
}
