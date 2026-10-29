#include "banner.h"
#include "display_driver.h"
#include "board_config.h"
#include "cjk_font.h"
#include "lvgl.h"
#include "text_utf8.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>
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

/* banner 几何：窗口高度为「一行 + 上下内边距」，左侧行情逐条上下滚动，
 * 右侧时间/标题逐行滚动；分栏竖线同时作为进度条。底部 2px 只留给评分/提交条。 */
#define BANNER_FONT_PX     18
#define BANNER_PAD_X       6
#define BANNER_PAD_Y       13         /* 行上下内边距：越大 banner 越高 */
#define BANNER_MAX_LINES   12
#define BANNER_MAX_GLYPHS  UI_BANNER_MAX_GLYPHS
#define BANNER_MAX_H       72          /* 背景缓冲最大高度（像素），需 >= 行高 + 2*BANNER_PAD_Y */
#define BANNER_BG_ALPHA    150         /* 0-255，banner 底色不透明度 */
#define BANNER_HOLD_MS     UI_STOCK_HOLD_MS   /* 每行/每条行情停留时间（毫秒） */
#define BANNER_SCROLL_MS   UI_STOCK_SCROLL_MS /* 行/行情切换滚动时间（毫秒） */

/* 左右分栏：左侧窄行情栏，右侧时间/标题；中间竖线兼作进度条。
 * 水平留白统一为 PAD_X（屏幕两侧）和 COL_GAP（竖线两侧）。 */
#define BANNER_STOCK_X0     BANNER_PAD_X
#define BANNER_STOCK_W      58
#define BANNER_COL_GAP      6         /* 竖线与左右两栏文字之间的间距 */
#define BANNER_DIVIDER_X    (BANNER_STOCK_X0 + BANNER_STOCK_W + BANNER_COL_GAP)
#define BANNER_DIVIDER_W    2
#define BANNER_TEXT_X0      (BANNER_DIVIDER_X + BANNER_DIVIDER_W + BANNER_COL_GAP)
#define BANNER_TEXT_W       (TFT_WIDTH - BANNER_PAD_X - BANNER_TEXT_X0)
#define BANNER_PROGRESS_COLOR 0x3476  /* 真实 RGB565，与旧底部进度条填充色一致 */

/* 行情字号略小于正文，一次只显示一个股票，名称和涨跌幅上下两行 */
#define BANNER_STOCK_FONT_PX 14
#define BANNER_STOCK_NAME_MAX 64
#define BANNER_TEXT_MAX      512
#define BANNER_STOCK_UP      0x07E0   /* 绿涨 */
#define BANNER_STOCK_DOWN    0xF800   /* 红跌 */
#define BANNER_STOCK_FLAT    0xFFFF

typedef struct {
    int16_t             x;       /* 屏幕 x（含左内边距） */
    int16_t             y_off;   /* 相对所在行顶部的 y 偏移 */
    uint16_t            w, h;    /* 字形位图尺寸 */
    uint16_t            stride;  /* A8 位图行跨距 */
    const uint8_t      *bmp;     /* A8 位图 */
    uint8_t             line;    /* 左侧标题行索引；行情字形忽略 */
    int8_t              stock_index; /* >=0 行情索引，-1 表示时间/标题 */
    uint16_t            color;   /* RGB565 前景色（未交换字节序） */
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
static int             s_banner_progress = 0;   /* 0..100，中间竖线进度 */

/* banner 窗口背景像素（已叠加半透明底色），供滚动重绘复用 */
static uint16_t        s_bg_buf[TFT_WIDTH * BANNER_MAX_H];
static bool            s_bg_valid      = false;

/* 左侧标题滚动动画状态 */
static int             s_cur_line        = 0;   /* 当前停留行 */
static int             s_scroll          = 0;   /* 0..line_h，当前滚动偏移 */
static bool            s_scrolling       = false;
static int64_t         s_scroll_start_us = 0;
static int64_t         s_hold_start_us   = 0;

/* 右侧股票栏独立滚动状态：一次显示一个股票，上下滚动切换 */
static int             s_stock_count      = 0;
static int             s_stock_line_h     = 0;
static int             s_stock_block_h    = 0;
static int             s_stock_block_top  = 0;
static int             s_stock_cur        = 0;
static int             s_stock_scroll     = 0;   /* 0..block_h */
static bool            s_stock_scrolling  = false;
static int64_t         s_stock_scroll_start_us = 0;
static int64_t         s_stock_hold_start_us   = 0;

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
    s_banner_progress    = 0;
    s_stock_count        = 0;
    s_stock_line_h       = 0;
    s_stock_block_h      = 0;
    s_stock_block_top    = 0;
    s_stock_cur          = 0;
    s_stock_scroll       = 0;
    s_stock_scrolling    = false;
}

/* 取一个字形位图并记录到 banner 字形表；颜色为未交换字节序的 RGB565 */
static bool banner_add_glyph(const lv_font_t *font, uint32_t cp, uint32_t next,
                             int x, int line, int8_t stock_index,
                             uint16_t color, int y_shift)
{
    if (s_banner_glyph_count >= BANNER_MAX_GLYPHS) return false;

    lv_font_glyph_dsc_t dsc;
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, next)) return false;
    if (dsc.box_w == 0 || dsc.box_h == 0) {
        lv_font_glyph_release_draw_data(&dsc);
        return false;   /* 空格等无位图字形 */
    }

    /* 内置字体需要调用方提供解压目标；Tiny TTF 返回自身缓存 */
    lv_draw_buf_t *owned = NULL;
    if (dsc.resolved_font->get_glyph_bitmap == lv_font_get_bitmap_fmt_txt) {
        owned = lv_draw_buf_create(dsc.box_w, dsc.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
        if (!owned) { lv_font_glyph_release_draw_data(&dsc); return false; }
    }
    const void *bmp = lv_font_get_glyph_bitmap(&dsc, owned);
    if (bmp == NULL) {
        lv_font_glyph_release_draw_data(&dsc);
        if (owned) lv_draw_buf_destroy(owned);
        return false;
    }
    const lv_draw_buf_t *db = (const lv_draw_buf_t *)bmp;

    banner_glyph_t *g = &s_banner_glyphs[s_banner_glyph_count++];
    g->x      = (int16_t)(x + dsc.ofs_x);
    g->y_off  = (int16_t)((font->line_height - font->base_line) - dsc.box_h - dsc.ofs_y + y_shift);
    g->w      = dsc.box_w;
    g->h      = dsc.box_h;
    g->stride = (uint16_t)db->header.stride;
    g->bmp    = db->data;
    g->line   = (uint8_t)line;
    g->stock_index = stock_index;
    g->color  = color;
    g->dsc    = dsc;
    g->owned  = owned;
    return true;
}

static int banner_text_width(const lv_font_t *font, const char *text)
{
    int width = 0;
    while (*text) {
        uint32_t cp = text_utf8_next(&text);
        width += lv_font_get_glyph_width(font, cp, 0);
    }
    return width;
}

/* 涨跌幅格式：+1.23% / -0.45% / 0.00% */
static void banner_format_change(int32_t bp, char *out, size_t size)
{
    const char *sign = bp > 0 ? "+" : (bp < 0 ? "-" : "");
    uint32_t value = bp < 0 ? (uint32_t)(-(int64_t)bp) : (uint32_t)bp;
    snprintf(out, size, "%s%u.%02u%%", sign, (unsigned)(value / 100), (unsigned)(value % 100));
}

static uint16_t banner_stock_color(int32_t bp)
{
    if (bp > 0) return BANNER_STOCK_UP;     /* 绿涨 */
    if (bp < 0) return BANNER_STOCK_DOWN;   /* 红跌 */
    return BANNER_STOCK_FLAT;
}

/* 取在 max_w 像素内能完整显示的最长前缀；按完整码点裁剪，不产生半个字符 */
static void banner_fit_text(const lv_font_t *font, const char *text, int max_w,
                            char *out, size_t size)
{
    if (!out || size == 0) return;
    out[0] = '\0';
    if (!text || max_w <= 0) return;

    int width = 0;
    size_t used = 0;
    for (const char *p = text; *p; ) {
        const char *q = p;
        uint32_t cp = text_utf8_next(&q);
        int adv = lv_font_get_glyph_width(font, cp, 0);
        if (width + adv > max_w) break;

        size_t n = (size_t)(q - p);
        if (used + n + 1 > size) break;
        memcpy(out + used, p, n);
        used += n;
        width += adv;
        p = q;
    }
    out[used] = '\0';
}

/* 从 x 开始放置一段文字，超出 limit 时按整字裁剪 */
static int banner_place_text(const lv_font_t *font, const char *text, int x, int limit,
                             int line, int8_t stock_index, uint16_t color, int y_shift,
                             int max_glyphs)
{
    int placed = 0;
    int pen = x;
    for (const char *p = text; *p && placed < max_glyphs; ) {
        const char *q = p;
        uint32_t cp = text_utf8_next(&q);
        int adv = lv_font_get_glyph_width(font, cp, 0);
        if (pen + adv > limit) break;

        uint32_t next = 0;
        if (*q) { const char *q2 = q; next = text_utf8_next(&q2); }
        /* 缺字形时跳过该字但继续后面的内容；表满时后续添加会自然失败 */
        if (banner_add_glyph(font, cp, next, pen, line, stock_index, color, y_shift)) placed++;
        pen += adv;
        p = q;
    }
    return placed;
}

/* 左侧股票窄栏：每次只显示一个股票，名称一行、涨跌幅一行，整体上下滚动 */
static void banner_place_stock_line(const lv_font_t *font,
                                    const banner_stock_t *stocks, int count,
                                    int x0, int width, int stock_line_h)
{
    if (count <= 0) return;
    const int cell_x = x0;
    const int cell_end = x0 + width;

    for (int i = 0; i < count; i++) {
        if (stocks[i].name == NULL) continue;

        char pct[16];
        banner_format_change(stocks[i].change_bp, pct, sizeof(pct));

        /* 名称按可用像素宽度裁剪后再居中，避免长名称溢出窄栏；
         * 选择显示多长的策略属于排版，不写回内容模型 */
        char name[BANNER_STOCK_NAME_MAX];
        banner_fit_text(font, stocks[i].name, width, name, sizeof(name));
        if (name[0] == '\0') continue;

        int name_w = banner_text_width(font, name);
        int name_x = cell_x + (width - name_w) / 2;
        if (name_x < cell_x) name_x = cell_x;
        int used = banner_place_text(font, name, name_x, cell_end,
                                     i, i, 0xFFFF, 0, UI_STOCK_NAME_GLYPHS);

        int pct_w = banner_text_width(font, pct);
        int pct_x = cell_x + (width - pct_w) / 2;
        if (pct_x < cell_x) pct_x = cell_x;
        banner_place_text(font, pct, pct_x, cell_end,
                          i, i, banner_stock_color(stocks[i].change_bp), stock_line_h,
                          UI_STOCK_GLYPHS - used);
    }
}

/* 分栏进度竖线的可见范围：0% 时占满 banner 高度，100% 时缩到中间消失 */
static bool banner_progress_range(int *top, int *bottom)
{
    if (s_banner_progress >= 100 || s_banner_h <= 0) return false;

    int height = (s_banner_h * (100 - s_banner_progress) + 99) / 100;
    if (height <= 0) return false;

    const int center = s_banner_y0 + s_banner_h / 2;
    *top    = center - height / 2;
    *bottom = *top + height - 1;
    return true;
}

/* 将当前进度竖线画到一行像素缓冲区 */
static void banner_draw_progress(uint16_t *row, int screen_y, int x_start, int width)
{
    if (!s_banner_active || row == NULL || width <= 0) return;

    int top, bottom;
    if (!banner_progress_range(&top, &bottom)) return;
    if (screen_y < top || screen_y > bottom) return;

    const uint16_t color = swap16(BANNER_PROGRESS_COLOR);
    for (int x = BANNER_DIVIDER_X; x < BANNER_DIVIDER_X + BANNER_DIVIDER_W; x++) {
        if (x < x_start || x >= x_start + width) continue;
        row[x - x_start] = color;
    }
}

/* 只重绘分栏竖线所在的窄列，避免进度变化时整屏刷新 */
static void banner_redraw_progress(void)
{
    if (!s_banner_active || !s_bg_valid || s_banner_h <= 0) return;

    for (int y = s_banner_y0; y <= s_banner_y1; y++) {
        int by = y - s_banner_y0;
        uint16_t row[BANNER_DIVIDER_W];
        for (int i = 0; i < BANNER_DIVIDER_W; i++) {
            row[i] = s_bg_buf[by * TFT_WIDTH + BANNER_DIVIDER_X + i];
        }
        banner_draw_progress(row, y, BANNER_DIVIDER_X, BANNER_DIVIDER_W);
        banner_write_row(BANNER_DIVIDER_X, y, row, BANNER_DIVIDER_W);
    }
}

void banner_set_progress(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == s_banner_progress) return;

    s_banner_progress = percent;
    banner_redraw_progress();
}

void banner_prepare(const banner_content_t *content)
{
    banner_release();

    if (content == NULL) return;

    const char *time  = content->time ? content->time : "";
    const char *title = content->title ? content->title : "";
    int stock_count = content->stocks ? content->stock_count : 0;
    if (stock_count < 0) stock_count = 0;
    if (stock_count > BANNER_STOCK_MAX) stock_count = BANNER_STOCK_MAX;
    if (time[0] == '\0' && title[0] == '\0' && stock_count == 0) return;

    const lv_font_t *font = cjk_font_get(BANNER_FONT_PX);
    if (font == NULL) font = LV_FONT_DEFAULT;

    const int line_h = font->line_height;
    if (line_h <= 0 || line_h > BANNER_MAX_H) {
        ESP_LOGW(TAG, "字体行高 %d 异常，标题浮层不可用", line_h);
        return;
    }

    /* banner 窗口几何：一行高 + 上下内边距，底部留 2px 给评分/提交条 */
    s_banner_line_h = line_h;
    s_banner_h      = line_h + 2 * BANNER_PAD_Y;
    if (s_banner_h > BANNER_MAX_H) s_banner_h = BANNER_MAX_H;
    s_banner_y1     = TFT_HEIGHT - 3;
    s_banner_y0     = s_banner_y1 - s_banner_h + 1;
    if (s_banner_y0 < 0) s_banner_y0 = 0;

    const int x_left = BANNER_TEXT_X0;
    const int max_w  = BANNER_TEXT_W;   /* 右侧时间/标题栏 */

    /* 第一遍：按常规文本规则换行（拉丁词整体不拆分，CJK 可逐字断行），
     * 并记录每个字的笔位置与所属行 */
    typedef struct { int16_t px; uint8_t line; const char *cp; } placed_t;
    static placed_t placed[UI_TITLE_GLYPHS];
    int placed_cnt = 0;
    int pen_x = x_left;
    int line  = 0;
    int pending_space_w = 0;   /* 上一个词之后累积的空白宽度（换行时丢弃） */

    /* 时间与标题分属不同行；标题内部仍按原有规则自动换行 */
    char text[BANNER_TEXT_MAX];
    size_t text_len = 0;
    text[0] = '\0';
    if (time[0]) {
        int n = snprintf(text, sizeof(text), "%s", time);
        if (n > 0 && (size_t)n < sizeof(text)) text_len = (size_t)n;
        else text[0] = '\0';
    }
    if (title[0]) {
        if (text_len > 0 && text_len + 1 < sizeof(text)) text[text_len++] = '\n';
        int n = snprintf(text + text_len, sizeof(text) - text_len, "%s", title);
        if (n < 0 || (size_t)n >= sizeof(text) - text_len) text_len = sizeof(text) - 1;
        else text_len += (size_t)n;
        text[text_len] = '\0';
    }

    const char *p = text;
    while (*p && placed_cnt < UI_TITLE_GLYPHS) {
        /* 显式换行 */
        {
            const char *q0 = p;
            uint32_t    c0 = text_utf8_next(&q0);
            if (c0 == '\n' || c0 == '\r') {
                p = q0;
                line++;
                pen_x = x_left;
                pending_space_w = 0;
                if (line >= BANNER_MAX_LINES) break;
                continue;
            }
            /* 空白：累积间距，留到下一个词前再决定是否换行 */
            if (text_utf8_is_space(c0)) {
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
            uint32_t    c = text_utf8_next(&q);
            if (text_utf8_is_cjk(c)) {
                unit_end = q;   /* 单个字 */
            } else {
                const char *s = p;
                while (*s) {
                    const char *t = s;
                    uint32_t    cc = text_utf8_next(&t);
                    if (text_utf8_is_space(cc) || text_utf8_is_cjk(cc) ||
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
            uint32_t c = text_utf8_next(&s);
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
        for (const char *s = unit_start; s < unit_end && placed_cnt < UI_TITLE_GLYPHS; ) {
            const char *t = s;
            uint32_t    c = text_utf8_next(&t);
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
    if (used_lines > BANNER_MAX_LINES) used_lines = BANNER_MAX_LINES;
    if (used_lines <= 0 && stock_count == 0) {
        return;
    }

    /* 左侧股票栏一次只显示一个股票，名称和涨跌幅分两行；先放入行情字形，
     * 避免超长标题占满字形表后把行情挤掉。 */
    if (stock_count > 0) {
        const lv_font_t *stock_font = cjk_font_get(BANNER_STOCK_FONT_PX);
        if (stock_font == NULL) stock_font = font;

        s_stock_count     = stock_count;
        s_stock_line_h    = stock_font->line_height;
        s_stock_block_h   = s_stock_line_h * 2;
        s_stock_block_top = s_banner_y0 + (s_banner_h - s_stock_block_h) / 2;
        if (s_stock_block_top < s_banner_y0) s_stock_block_top = s_banner_y0;

        banner_place_stock_line(stock_font, content->stocks, stock_count,
                                BANNER_STOCK_X0, BANNER_STOCK_W, s_stock_line_h);
    }

    /* 第二遍：取字形位图，记录窗口内相对坐标（行内偏移 + 行索引） */
    for (int i = 0; i < placed_cnt; i++) {
        const char *q  = placed[i].cp;
        uint32_t    cp = text_utf8_next(&q);
        uint32_t    next = 0;
        if (*q) { const char *q2 = q; next = text_utf8_next(&q2); }
        banner_add_glyph(font, cp, next, placed[i].px, placed[i].line, -1, 0xFFFF, 0);
    }

    if (s_banner_glyph_count == 0) {
        return;
    }

    s_banner_line_count = used_lines > 0 ? used_lines : 1;
    s_banner_active     = true;
    s_banner_capture    = true;    /* 首帧解码图片时捕获背景 */
    s_cur_line          = 0;
    s_scroll            = 0;
    s_scrolling         = false;

    /* 预填黑色，避免图片未覆盖区域出现脏数据 */
    memset(s_bg_buf, 0, sizeof(s_bg_buf));

    ESP_LOGI(TAG, "标题浮层排版：%d 字形，%d 行，行情 %d 个，窗口 y=%d..%d（行高 %d）",
             s_banner_glyph_count, s_banner_line_count, stock_count,
             s_banner_y0, s_banner_y1, s_banner_line_h);
}

/* 将右侧当前行（滚动时叠加下一行）和左侧滚动行情混合到一行缓冲区 */
static void banner_draw_glyphs(uint16_t *row, int screen_y, int x_start, int width)
{
    if (!s_banner_active || s_banner_line_h <= 0) return;
    if (screen_y < s_banner_y0 || screen_y > s_banner_y1) return;

    const int base_cur = s_banner_y0 + BANNER_PAD_Y - s_scroll;
    const int base_nxt = base_cur + s_banner_line_h;
    const int cur = s_cur_line;
    const int nxt = s_scrolling ? ((s_cur_line + 1) % s_banner_line_count) : -1;

    const int stock_nxt = (s_stock_scrolling && s_stock_count > 0)
        ? ((s_stock_cur + 1) % s_stock_count) : -1;
    const int stock_base_cur = s_stock_block_top - s_stock_scroll;
    const int stock_base_nxt = stock_base_cur + s_stock_block_h;

    for (int gi = 0; gi < s_banner_glyph_count; gi++) {
        const banner_glyph_t *g = &s_banner_glyphs[gi];
        int base;
        if (g->stock_index >= 0) {
            if (g->stock_index == s_stock_cur) {
                base = stock_base_cur;
            } else if (stock_nxt >= 0 && g->stock_index == stock_nxt) {
                base = stock_base_nxt;
            } else {
                continue;
            }
        } else if ((int)g->line == cur) {
            base = base_cur;
        } else if (nxt >= 0 && (int)g->line == nxt) {
            base = base_nxt;
        } else {
            continue;
        }

        int ly = screen_y - (base + g->y_off);
        if (ly < 0 || ly >= g->h) continue;

        /* 字形颜色按 RGB565 存储，混合前展开成 8 位分量 */
        uint8_t cr = (g->color >> 11) & 0x1F;
        uint8_t cg = (g->color >> 5) & 0x3F;
        uint8_t cb = g->color & 0x1F;
        cr = (uint8_t)((cr << 3) | (cr >> 2));
        cg = (uint8_t)((cg << 2) | (cg >> 4));
        cb = (uint8_t)((cb << 3) | (cb >> 2));

        const uint8_t *line = g->bmp + (size_t)ly * g->stride;
        for (int col = 0; col < g->w; col++) {
            int x = g->x + col;
            if (x < x_start || x >= x_start + width) continue;
            uint8_t a = line[col];
            if (a == 0) continue;
            uint16_t real = swap16(row[x - x_start]);
            real = blend565(real, cr, cg, cb, a);
            row[x - x_start] = swap16(real);
        }
    }
}

void banner_apply_row(uint16_t *row, int screen_y, int x_start, int width)
{
    if (row == NULL || width <= 0 || screen_y < 0 || screen_y >= TFT_HEIGHT) {
        return;
    }

    /* 1) 半透明深色 banner 背景（banner 激活时绘制），
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

    /* 2) 标题与行情文字（按字形 alpha 和各自前景色混合） */
    banner_draw_glyphs(row, screen_y, x_start, width);

    /* 3) 分栏进度竖线；背景缓存已在步骤 1 捕获，不含该竖线 */
    banner_draw_progress(row, screen_y, x_start, width);
}

/* 用缓存的背景底图 + 当前滚动偏移重绘整个 banner 窗口 */
static void banner_redraw(void)
{
    static uint16_t row[TFT_WIDTH];
    for (int y = s_banner_y0; y <= s_banner_y1; y++) {
        int by = y - s_banner_y0;
        memcpy(row, &s_bg_buf[by * TFT_WIDTH], sizeof(uint16_t) * TFT_WIDTH);
        banner_draw_glyphs(row, y, 0, TFT_WIDTH);
        banner_draw_progress(row, y, 0, TFT_WIDTH);
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

    s_stock_cur        = 0;
    s_stock_scroll     = 0;
    s_stock_scrolling  = false;
    /* 股票滚动与标题错开半个停留周期，避免两个栏同时滚动 */
    s_stock_hold_start_us = s_hold_start_us - (int64_t)BANNER_HOLD_MS * 1000 / 2;
    s_stock_scroll_start_us = s_stock_hold_start_us;
}

void banner_tick(void)
{
    if (!s_banner_active || !s_bg_valid) return;

    const int64_t now = esp_timer_get_time();
    bool redraw = false;

    /* 右侧时间/标题逐行滚动 */
    if (s_banner_line_count > 1) {
        if (s_scrolling) {
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
            redraw = true;
        } else if (now - s_hold_start_us >= (int64_t)BANNER_HOLD_MS * 1000) {
            s_scrolling       = true;
            s_scroll_start_us = now;
            s_scroll          = 0;
        }
    }

    /* 左侧股票栏一次一个，按整个名称+涨跌幅块上下滚动 */
    if (s_stock_count > 1) {
        if (s_stock_scrolling) {
            int64_t el = now - s_stock_scroll_start_us;
            if (el >= (int64_t)BANNER_SCROLL_MS * 1000) {
                s_stock_cur       = (s_stock_cur + 1) % s_stock_count;
                s_stock_scroll    = 0;
                s_stock_scrolling = false;
                s_stock_hold_start_us = now;
            } else {
                s_stock_scroll = (int)((int64_t)s_stock_block_h * el /
                                       ((int64_t)BANNER_SCROLL_MS * 1000));
            }
            redraw = true;
        } else if (now - s_stock_hold_start_us >= (int64_t)BANNER_HOLD_MS * 1000) {
            s_stock_scrolling       = true;
            s_stock_scroll_start_us = now;
            s_stock_scroll          = 0;
        }
    }

    if (redraw) banner_redraw();
}

void banner_show_solid(uint32_t bg_rgb24, const banner_content_t *content)
{
    banner_prepare(content);

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
