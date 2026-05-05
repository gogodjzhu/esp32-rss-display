# Design: submit-rating-api

## 架构概览

```
main.cpp                  image_fetcher.c           后端
────────────────────────────────────────────────────────────
image_fetcher_get_next_url()
                          GET /v1/device/{id}/next
                          ← { image_url, item_id, ... }
                          内部缓存 s_current_item_id
          ↓
[显示图片，用户交互评分]
          ↓
image_fetcher_submit_rating(sel)
                          POST /v1/item/{item_id}/rating
                          { "rating": sel,
                            "device_id": DEVICE_ID }
                          ← 200 OK / 非 200
          ↓ esp_err_t
ui_animation_show_submit_bar(success)
  底部 2px 全绿/全红
vTaskDelay(1000ms)
清空按钮队列
恢复进度条，退出评价模式
```

---

## 状态机变更（main.cpp）

评价模式退出路径从 1 个变为 2 个，新增 `DISPLAY_SUBMITTING` 中间状态以避免 1 秒延迟期间处理按钮事件：

```
DISPLAY_NORMAL
    │ 双击
    ▼
DISPLAY_RATING
    │ 5秒无操作
    ▼
[image_fetcher_submit_rating()]  ← 阻塞 ~0.5-5秒
    │
    ├─ ESP_OK  → show_submit_bar(true)
    └─ ESP_FAIL → show_submit_bar(false)
    │
    ▼
vTaskDelay(1000ms)
清空 s_button_queue（drain）
ui_animation_update_bottom_bar(t_saved * 100 / total_steps)
state = DISPLAY_NORMAL
t = t_saved
```

`DISPLAY_SUBMITTING` 实际上通过顺序执行实现（阻塞调用），不需要新的枚举值；任务在此期间不处理队列。

---

## image_fetcher 模块变更

### 新增内部状态

```c
static uint32_t s_current_item_id = 0;  /* 当前展示条目 ID，0 表示未获取 */
```

### `image_fetcher_get_next_url` 扩展

在现有 `image_url` 字段解析之后，额外解析 `item_id` 字段：

```c
/* 期望 JSON 格式：{ "item_id": 203, "image_url": "http://...", ... } */
const char *id_key = "\"item_id\"";
char *id_pos = strstr(ctx.buf, id_key);
if (id_pos) {
    char *colon = strchr(id_pos + strlen(id_key), ':');
    if (colon) {
        s_current_item_id = (uint32_t)strtoul(colon + 1, NULL, 10);
    }
} else {
    s_current_item_id = 0;  /* 解析失败，重置为 0 */
}
```

### 新增函数 `image_fetcher_submit_rating`

```c
esp_err_t image_fetcher_submit_rating(int rating)
```

- `s_current_item_id == 0`：直接返回 `ESP_FAIL`，打印警告日志
- 否则：构造 URL `{BACKEND_URL}/v1/item/{id}/rating`，POST JSON body
- `timeout_ms = 5000`
- HTTP 状态码 200-299 返回 `ESP_OK`，其余返回 `ESP_FAIL`

### `.h` 新增声明

```c
esp_err_t image_fetcher_submit_rating(int rating);
```

---

## ui_animation 模块变更

### 新增函数 `ui_animation_show_submit_bar`

```c
void ui_animation_show_submit_bar(bool success)
```

复用现有 `ui_animation_write_row` 机制，底部 2px（y:238, y:239）全行填充：
- `success = true`：`0x2ECC71`（绿）
- `success = false`：`0xE74C3C`（红）

颜色值需做 RGB565 字节序交换（与现有 `show_rating_bar` 一致）。

---

## main.cpp 变更

### `submit_rating` 函数替换

```c
// 删除原有占位实现，替换为：
static void do_submit_and_show(int sel)
{
    esp_err_t err = image_fetcher_submit_rating(sel);
    bool success = (err == ESP_OK);
    ui_animation_show_submit_bar(success);
    ESP_LOGI(TAG, "评价提交%s：%d 分", success ? "成功" : "失败", sel);
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* 清空评价期间积压的按钮事件 */
    { int64_t dummy; while (xQueueReceive(s_button_queue, &dummy, 0) == pdTRUE) {} }
}
```

在 5 秒超时触发点调用 `do_submit_and_show(sel)` 替代原 `submit_rating(sel)`，之后恢复进度条和 `t = t_saved`。

---

## 关键参数

| 参数 | 值 | 说明 |
|------|----|------|
| HTTP timeout | 5000ms | 避免弱网长时间冻屏 |
| 状态显示时长 | 1000ms | 足够用户看清结果 |
| 成功颜色 | `0x2ECC71` | 绿，与评分条 5 分一致 |
| 失败颜色 | `0xE74C3C` | 红，与评分条 1 分一致 |
| item_id=0 时 | 直接 FAIL | 不发请求，防止污染数据 |
