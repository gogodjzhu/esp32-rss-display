#ifndef UI_LIMITS_H
#define UI_LIMITS_H

/*
 * 显示预算的唯一来源。
 *
 * 一次内容刷新期间能展示多少条行情，由展示时长和每条行情的滚动节奏决定；
 * 行情的 JSON 解析上限、banner 数组和字型表都从这个结果推导，避免多处手写
 * 常量不一致。修改下面的时长、滚动节奏或 UI_STOCK_LIMIT 即可整体调整。
 */

/* 每条内容的展示时长（毫秒） */
#define UI_DISPLAY_MS        60000
/* 每条行情的停留时间和上下滚动动画时长（毫秒） */
#define UI_STOCK_HOLD_MS     2500
#define UI_STOCK_SCROLL_MS   450
#define UI_STOCK_SLOT_MS     (UI_STOCK_HOLD_MS + UI_STOCK_SCROLL_MS)

/* 按展示周期推导的最大行情条数：当前为 60000 / 2950 = 20 条 */
#define UI_STOCK_MAX_BY_TIME (UI_DISPLAY_MS / UI_STOCK_SLOT_MS)

/* 0 表示按展示周期自动推导；需要限制 RAM 或展示节奏时填更小的固定值。
 * 填写的值不会超过按时间推导的上限。 */
#define UI_STOCK_LIMIT       0

#if UI_STOCK_LIMIT > 0
#define UI_STOCK_MAX (UI_STOCK_LIMIT < UI_STOCK_MAX_BY_TIME ? UI_STOCK_LIMIT : UI_STOCK_MAX_BY_TIME)
#else
#define UI_STOCK_MAX UI_STOCK_MAX_BY_TIME
#endif

/*
 * 字型表预算：右侧标题/时间区域保留固定容量，左侧每条行情按最坏情况预留。
 * 行情名按 58px 宽度裁剪，单条最多约 10 个名称字形；涨跌幅最多约 8 个字形。
 */
#define UI_TITLE_GLYPHS       128
#define UI_STOCK_NAME_GLYPHS  10
#define UI_STOCK_GLYPHS       18
#define UI_BANNER_MAX_GLYPHS  (UI_TITLE_GLYPHS + UI_STOCK_MAX * UI_STOCK_GLYPHS)

_Static_assert(UI_STOCK_MAX >= 1, "展示周期不足以滚动一条行情");
_Static_assert(UI_STOCK_MAX * UI_STOCK_SLOT_MS <= UI_DISPLAY_MS,
               "行情条数超过一次展示周期可完整滚完的数量");

#endif
