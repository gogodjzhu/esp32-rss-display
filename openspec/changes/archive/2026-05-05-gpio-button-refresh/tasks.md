# Tasks: gpio-button-refresh

## Task 1: 在 main.cpp 中添加 GPIO9 按钮初始化

- 在 `src/main.cpp` 顶部添加 `#include <driver/gpio.h>`
- 声明全局 task handle：`static TaskHandle_t s_display_task_handle = NULL;`
- 实现 ISR 函数 `gpio_button_isr`（带 300ms 防抖，使用 `esp_timer_get_time()`，调用 `xTaskNotifyFromISR`）
- 实现 `button_init()` 函数（配置 GPIO9 为输入、内部上拉、下降沿中断，安装 ISR service，注册 handler）

## Task 2: 修改 image_display_task 保存 task handle

- 在 `image_display_task` 函数开头（`image_fetcher_init()` 之后）添加：
  ```c
  s_display_task_handle = xTaskGetCurrentTaskHandle();
  ```

## Task 3: 改造倒计时循环以响应按钮通知

- 将原有倒计时 `for` 循环中的 `vTaskDelay` 替换为 `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PROGRESS_UPDATE_MS))`
- 进入循环前调用 `ulTaskNotifyTake(pdTRUE, 0)` 清除残留通知
- 若 `ulTaskNotifyTake` 返回值 > 0，立即 `break` 跳出循环

## Task 4: 在 app_main 中调用 button_init

- 在 `app_main` 中，WiFi 连接成功后（`http_server_start()` 之前）调用 `button_init()`

## Task 5: 编译验证

- 运行 `pio run` 确认编译通过，无错误或警告

## Task 6: 烧录并功能验证

- 运行 `pio run --target upload` 烧录到设备
- 等待设备启动并显示第一张图片
- 按下 GPIO9 按钮，确认图片立即切换（无需等待倒计时结束）
- 拍摄屏幕照片确认显示正常
