# Tasks: submit-rating-api

## Task 1: image_fetcher — 解析 item_id，缓存到静态变量

文件：`lib/image_fetcher/image_fetcher.c`

- [x] 在文件顶部（静态变量区）添加：
  ```c
  static uint32_t s_current_item_id = 0;  /* 当前展示条目 ID，0 表示未获取 */
  ```
- [x] 在 `image_fetcher_get_next_url` 中，`image_url` 解析成功后，追加解析 `item_id`：
  ```c
  /* 解析 item_id 字段（整数） */
  const char *id_key = "\"item_id\"";
  char *id_pos = strstr(ctx.buf, id_key);
  if (id_pos) {
      char *colon = strchr(id_pos + strlen(id_key), ':');
      if (colon) {
          s_current_item_id = (uint32_t)strtoul(colon + 1, NULL, 10);
          ESP_LOGI(TAG, "item_id: %" PRIu32, s_current_item_id);
      } else {
          s_current_item_id = 0;
      }
  } else {
      ESP_LOGW(TAG, "JSON 中无 item_id 字段，评分提交将被跳过");
      s_current_item_id = 0;
  }
  ```
  需要 `#include <inttypes.h>` 用于 `PRIu32` ✓已添加

## Task 2: image_fetcher — 新增 submit_rating HTTP 函数

文件：`lib/image_fetcher/image_fetcher.c`，`lib/image_fetcher/image_fetcher.h`

- [x] 在 `.h` 中新增声明：
  ```c
  esp_err_t image_fetcher_submit_rating(int rating);
  ```

- [x] 在 `.c` 中实现（放在 `image_fetcher_download_and_show` 之后）：
  - `s_current_item_id == 0` 或 `rating == 0`：打印警告，返回 `ESP_FAIL`
  - 构造 URL：`{BACKEND_URL}/v1/item/{s_current_item_id}/rating`
  - 构造 JSON body：`{"rating":<N>,"device_id":"<DEVICE_ID>"}`（使用 `snprintf`，无需第三方库）
  - `esp_http_client` POST 请求，`timeout_ms = 5000`，`Content-Type: application/json`
  - HTTP 状态码 200-299 返回 `ESP_OK`，其余返回 `ESP_FAIL`
  - 任何 `esp_err_t` 错误返回 `ESP_FAIL`

## Task 3: ui_animation — 新增提交状态栏函数

文件：`lib/ui_animation/ui_animation.c`，`lib/ui_animation/ui_animation.h`

- [x] 在 `.h` 中新增声明：
  ```c
  void ui_animation_show_submit_bar(bool success);
  ```
  `#include <stdbool.h>` 已添加到 `.h`

- [x] 在 `.c` 中实现（放在 `ui_animation_show_rating_bar` 之后）：
  - 复用 `RGB24_TO_565_SWAPPED` 宏（或直接内联）
  - 全行填充对应颜色（320px 宽）
  - 写 y:238 和 y:239 两行

## Task 4: main.cpp — 替换 submit_rating，加入状态显示逻辑

文件：`src/main.cpp`

- [x] 删除现有 `submit_rating(int sel)` 占位函数

- [x] 新增 `do_submit_and_show(int sel)` 函数（在 `image_display_task` 之前）

- [x] 在 5 秒超时处理中将 `submit_rating(sel)` 替换为 `do_submit_and_show(sel)`

## Task 5: 编译验证

- [x] 运行 `pio run` 确认编译通过，无错误

## Task 6: 烧录并功能验证

- [x] 运行 `pio run --target upload` 烧录成功
- [ ] 在有网络的情况下：双击进入评价，选择评分，等待 5 秒超时提交
  - 串口确认 `POST /v1/item/{id}/rating` 发出
  - 屏幕底部显示 1 秒绿色，随后恢复蓝色进度条
- [ ] 后端日志或数据库确认评分已写入
- [ ] 测试 `item_id=0` 场景（如有方法模拟）：应显示红色状态栏
