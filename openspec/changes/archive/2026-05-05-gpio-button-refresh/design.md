# Design: gpio-button-refresh

## 方案概览

在 `image_display_task` 的倒计时循环中，通过 FreeRTOS 任务通知（`TaskNotify`）响应 GPIO9 中断，从而中断等待并立即进入下一轮图片拉取。

## GPIO9 硬件特性

| 参数 | 值 |
|------|-----|
| GPIO 编号 | 9 |
| 有效电平 | 低（LOW）— 按下时拉低 |
| 内部上拉 | 是（`GPIO_PULLUP_ONLY`） |
| 中断触发 | 下降沿（`GPIO_INTR_NEGEDGE`） |

## 中断 → 任务通知机制

```
GPIO9 中断（ISR）
    └── xTaskNotifyFromISR(image_display_task_handle, ...)
            └── image_display_task 唤醒，退出倒计时循环
                    └── 立即执行下一次图片拉取
```

使用 `xTaskNotifyFromISR` / `ulTaskNotifyTake` 是 ESP-IDF FreeRTOS 在中断与任务间通信的推荐方式，无需额外队列或信号量。

## 防抖策略

在 ISR 中记录上次触发时间戳，若距上次触发不足 300ms 则忽略，防止机械抖动产生多次触发。使用 `esp_timer_get_time()` 获取微秒时间戳（ISR 安全）。

## 代码结构变更

### `src/main.cpp`

1. 声明 `static TaskHandle_t s_display_task_handle = NULL;`
2. 新增 ISR 函数：
   ```c
   static void IRAM_ATTR gpio_button_isr(void *arg) {
       // 防抖：距上次 < 300ms 忽略
       static int64_t last_us = 0;
       int64_t now = esp_timer_get_time();
       if (now - last_us < 300000) return;
       last_us = now;
       // 通知任务
       BaseType_t woken = pdFALSE;
       xTaskNotifyFromISR(s_display_task_handle, 1, eSetValueWithOverwrite, &woken);
       portYIELD_FROM_ISR(woken);
   }
   ```
3. 新增初始化函数 `button_init()`：
   ```c
   static void button_init(void) {
       gpio_config_t cfg = {
           .pin_bit_mask = (1ULL << 9),
           .mode = GPIO_MODE_INPUT,
           .pull_up_en = GPIO_PULLUP_ENABLE,
           .pull_down_en = GPIO_PULLDOWN_DISABLE,
           .intr_type = GPIO_INTR_NEGEDGE,
       };
       gpio_config(&cfg);
       gpio_install_isr_service(0);
       gpio_isr_handler_add(GPIO_NUM_9, gpio_button_isr, NULL);
   }
   ```
4. 修改 `image_display_task`：
   - 保存 task handle：`s_display_task_handle = xTaskGetCurrentTaskHandle();`
   - 将倒计时 `for` 循环改为逐步 `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PROGRESS_UPDATE_MS))`，返回非零说明按钮触发，立即 `break`
5. 在 `app_main` 中，WiFi 连接成功后调用 `button_init()`

## 倒计时循环改造

当前：
```c
for (int t = 0; t <= total_steps; t++) {
    ui_animation_update_bottom_bar(t * 100 / total_steps);
    vTaskDelay(pdMS_TO_TICKS(PROGRESS_UPDATE_MS));
}
```

改后：
```c
s_display_task_handle = xTaskGetCurrentTaskHandle();
ulTaskNotifyTake(pdTRUE, 0);  // 清除残留通知
for (int t = 0; t <= total_steps; t++) {
    ui_animation_update_bottom_bar(t * 100 / total_steps);
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PROGRESS_UPDATE_MS));
    if (notified > 0) break;  // 按钮触发，立即跳出
}
```

## 不引入新任务

所有逻辑在现有 `image_display_task` 中完成，不增加 FreeRTOS 任务数量。
