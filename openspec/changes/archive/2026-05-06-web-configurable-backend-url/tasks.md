# Tasks: web-configurable-backend-url

## Task 1: image_fetcher — 从 NVS 动态加载 backend_url

文件：`lib/image_fetcher/image_fetcher.c`

- [x] 将文件顶部 `#define BACKEND_URL CONFIG_BACKEND_URL` 替换为静态变量：
  ```c
  static char s_backend_url[128];
  ```
- [x] 在 `image_fetcher_init()` 中，初始化 `s_backend_url` 为 Kconfig 默认值，再尝试从 NVS 覆盖：
  ```c
  strncpy(s_backend_url, CONFIG_BACKEND_URL, sizeof(s_backend_url) - 1);
  char nvs_url[128] = {0};
  size_t nvs_url_len = sizeof(nvs_url);
  if (nvs_manager_get_str("backend_url", nvs_url, &nvs_url_len) == ESP_OK && strlen(nvs_url) > 0) {
      strncpy(s_backend_url, nvs_url, sizeof(s_backend_url) - 1);
      ESP_LOGI(TAG, "从 NVS 加载 backend_url: %s", s_backend_url);
  } else {
      ESP_LOGI(TAG, "使用默认 backend_url: %s", s_backend_url);
  }
  ```
- [x] 在 `image_fetcher_init()` 的日志行中将 `BACKEND_URL` 替换为 `s_backend_url`
- [x] 将 `snprintf(api_url, ...)` 中的 `BACKEND_URL` 替换为 `s_backend_url`（`/next` 请求处）
- [x] 将 rating URL 构造中的 `BACKEND_URL` 替换为 `s_backend_url`（`/rating` 请求处）
- [x] 添加 `#include "nvs_manager.h"`（如未包含）

## Task 2: http_server — 新增 settings 相关 URI handler

文件：`lib/http_server/http_server.c`

- [x] 在文件顶部 include 区添加 `#include "nvs_manager.h"`（如未包含）
- [x] 新增 `settings_get_handler`（服务 `settings.html`）
- [x] 新增 `api_settings_get_handler`（返回当前 backend_url JSON，NVS 优先，fallback CONFIG_BACKEND_URL）
- [x] 新增 `api_settings_post_handler`（解析 JSON body，保存到 NVS，延迟 2s 重启）
- [x] 在 URI 路由定义区新增三个 `httpd_uri_t` 常量：
  - `settings_uri` — GET /settings
  - `api_settings_get_uri` — GET /api/settings
  - `api_settings_post_uri` — POST /api/settings
- [x] 在 `http_server_start()` 中，`httpd_config_t` 初始化后显式设置 `config.max_uri_handlers = 12`
- [x] 在通配符 handler 注册之前注册以上三个 URI

## Task 3: data/www — 新增 settings.html

文件：`data/www/settings.html`（新文件）

- [x] 创建 `settings.html`，复用 `styles.css` 的 `.card`、`input`、`button`、`.back` 样式
- [x] 页面加载时 `fetch('/api/settings')` 获取当前值并预填输入框
- [x] 表单提交时 `fetch('/api/settings', {method:'POST', body: JSON.stringify({backend_url: val})})` 发送
- [x] 成功响应后隐藏表单，显示"保存成功，正在重启..."提示
- [x] 页面底部提供"← 返回"链接（`href="/"`）

## Task 4: data/www — status.html 新增 settings 入口

文件：`data/www/status.html`

- [x] 在现有按钮组中新增跳转按钮：
  ```html
  <button class="btn-secondary" onclick="location.href='/settings'">RSS 新闻配置</button>
  ```
  位置：放在 "Configure WiFi" 按钮之后

## Task 5: 编译验证

- [x] 运行 `pio run` 确认编译通过，无警告/错误

## Task 6: 烧录并功能验证

- [ ] 运行 `pio run --target upload` 烧录成功
- [ ] 设备重启后，访问 `http://<device-ip>/settings`，确认显示当前 backend URL
- [ ] 修改 URL 并保存，串口确认保存日志，设备重启
- [ ] 重启后访问 `/settings` 确认显示已更新的 URL（NVS 持久化验证）
- [ ] 串口确认 `image_fetcher_init` 日志输出新的 backend_url
- [ ] 访问 `http://<device-ip>/`（STA 模式首页），确认有"RSS 新闻配置"按钮
