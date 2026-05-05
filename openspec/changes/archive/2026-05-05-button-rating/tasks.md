# Tasks: button-rating

## Task 1: 修改 ISR —— 防抖改 50ms，改为向队列发送时间戳

文件：`src/main.cpp`

- [x] 添加 `#include <freertos/queue.h>`
- [x] 声明全局队列：`static QueueHandle_t s_button_queue = NULL;`
- [x] 修改 `gpio_button_isr`：防抖阈值 50ms，改为 `xQueueSendFromISR` 发送 `int64_t` 时间戳
- [x] 在 `button_init()` 中创建队列：`s_button_queue = xQueueCreate(4, sizeof(int64_t))`

## Task 2: 实现评价指示条函数

文件：`lib/ui_animation/ui_animation.h`，`lib/ui_animation/ui_animation.c`

- [x] 在 `ui_animation.h` 中添加声明：`void ui_animation_show_rating_bar(int sel)`
- [x] 实现 `ui_animation_show_rating_bar(int sel)`：
  - `sel=0`：全暗灰（无评价）
  - `sel=1-5`：底部 2px 分 5 段（每段 64px），前 N 段点亮，颜色红→橙→黄→浅绿→深绿
  - 直接写 TFT 底部两行（y:238, y:239），不触碰图片像素
  - 颜色做 RGB565 字节序交换（`LV_COLOR_FORMAT_RGB565_SWAPPED`）
- [x] 删除旧版 `show_rating_overlay` / `clear_rating_overlay` 函数

## Task 3: 改造 image_display_task —— 状态机、双击检测、单击跳图、评价模式防抖

文件：`src/main.cpp`

- [x] 添加状态变量：`state`、`sel`（int 0-5）、`waiting_second`、`first_click_us`、`last_action_us`、`last_rating_act_us`、`t_saved`
- [x] 清空启动时残留按钮事件（drain queue）
- [x] 倒计时循环改为 `xQueueReceive` 轮询
- [x] NORMAL 模式双击逻辑（400ms 窗口）：进入评价模式，暂停倒计时，显示 rating_bar
- [x] NORMAL 模式单击逻辑：双击窗口超时后（轮询时检查）执行 `break` 跳到下一张图片
- [x] RATING 模式单击：200ms 防抖，循环 0→1→2→3→4→5→0，更新 rating_bar
- [x] 5 秒无操作超时：提交评价，退出评价模式，恢复进度条，从 `t_saved` 继续倒计时

## Task 4: 添加 submit_rating 占位函数

文件：`src/main.cpp`

- [x] `submit_rating(int sel)`：sel=0 打印"无评价"，sel=1-5 打印对应分数
- [x] TODO 注释标记后续替换为远端 HTTP 调用

## Task 5: 编译验证

- [x] `pio run` 编译通过，无错误

## Task 6: 烧录并功能验证

- [x] `pio run --target upload` 烧录成功
- [x] 串口日志确认设备正常启动（WiFi 连接、图片拉取、显示）
- [x] 用户确认功能正常（单击跳图、双击进入评价模式、评分切换、防抖）
