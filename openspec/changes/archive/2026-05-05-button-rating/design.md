# Design: button-rating

## 按钮事件流

ISR 职责缩减到最小，只做防抖和入队，所有手势判断在任务上下文完成。

```
GPIO9 下降沿
     │
     ▼
┌─────────────────────────────┐
│  gpio_button_isr (IRAM)     │
│  防抖：50ms                  │
│  xQueueSendFromISR(timestamp)│
└──────────────┬──────────────┘
               │
               ▼
    button_event_queue (深度 4)
               │
               │ xQueueReceive（在 image_display_task 中）
               ▼
    双击检测：收到第1次点击仅记录时间戳，等待第2次
              第2次到来时检查间隔 < 400ms → 双击
              否则第2次点击视为新的第1次，继续等待
```

## 状态机

`image_display_task` 内部维护两层状态：

```
外层（display_state）：

  NORMAL ──双击──▶ RATING
  RATING ──5秒超时──▶ NORMAL（提交评价）

内层（rating_selection，仅在 RATING 时有效）：

  NONE ──单击──▶ LIKE ──单击──▶ DISLIKE ──单击──▶ NONE
```

**评价模式行为细节：**
- 进入时：记录剩余倒计时步数（`t_saved`）、切换底部进度条为评价指示条
- 每次单击：循环切换 `rating_selection`，更新指示条颜色
- 超时提交：调用 `submit_rating()`（当前只打印日志），恢复底部进度条
- 退出时：从 `t_saved` 继续倒计时（保留剩余时间）

## 时间参数

| 参数 | 值 | 说明 |
|------|----|------|
| ISR 防抖 | 50ms | 消除机械抖动，足够识别双击 |
| 双击时间窗口 | 400ms | 两次点击间隔 |
| 评价超时 | 5000ms | 最后一次操作后 5 秒自动提交 |
| 进度条更新间隔 | 100ms | 不变，评价模式下暂停更新 |

## UI：底部评价指示条

评价模式复用底部 2px 进度条区域（y:238~239, 宽 320px），完全不碰图片像素。
退出评价模式后恢复蓝色进度条。

```
屏幕布局（320×240）：

┌─────────────────────────────────────────┐  y=0
│                                         │
│           图片区域                       │
│       (直接写TFT, y:0~219)               │
│                                         │
├─────────────────────────────────────────┤  y=220
│       （空白区域，y:220~237）             │
├─────────────────────────────────────────┤  y=238
│   底部 2px 区域（LVGL / 直接写TFT）      │
└─────────────────────────────────────────┘  y=240

NORMAL 模式（LVGL bar）：
├────────────────────────────────────────┤  蓝色进度条（现有）

RATING 模式（直接写 TFT，绕过 LVGL）：
├───────────────────┬────────────────────┤
│   左半 (0~159)    │   右半 (160~319)   │
│   不喜欢区域       │   喜欢区域          │
└───────────────────┴────────────────────┘

NONE:    左暗(0x333333) + 右暗(0x333333)
LIKE:    左暗(0x333333) + 右亮绿(0x2ECC71)
DISLIKE: 左亮红(0xE74C3C) + 右暗(0x333333)
```

## 函数签名

### ui_animation.h / ui_animation.c

```c
/* 在底部 2px 区域绘制评价指示条（直接写 TFT，绕过 LVGL）
 * sel: 0=NONE（两段均暗）, 1=LIKE（右半亮绿）, 2=DISLIKE（左半亮红） */
void ui_animation_show_rating_bar(int sel);
```

`ui_animation_update_bottom_bar` 在退出评价模式时照常调用即可恢复进度条。

### main.cpp

```c
/* 评价提交占位函数（当前只打印日志，后续替换为 HTTP 调用） */
static void submit_rating(int sel);
```

## 双击检测逻辑（修正版）

```c
// 状态：等待第一次点击还是第二次点击
bool waiting_second = false;
int64_t first_click_us = 0;

// 收到点击时：
if (!waiting_second) {
    // 第一次点击，记录时间，进入等待第二次状态
    waiting_second = true;
    first_click_us = ts;
} else {
    int64_t interval = ts - first_click_us;
    if (interval < 400000LL) {
        // 双击确认
        waiting_second = false;
        // → 进入评价模式
    } else {
        // 间隔太长，第二次点击视为新的第一次
        first_click_us = ts;
        // waiting_second 保持 true，继续等待
    }
}
```

> 注：`waiting_second` 标志跨越 `for` 循环迭代持久存在，单击动作（跳图）仅在评价模式下无效，NORMAL 模式下第一次点击本身不触发任何动作，等待第二次确认。
