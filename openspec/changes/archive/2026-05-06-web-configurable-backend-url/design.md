# Design: web-configurable-backend-url

## 架构概览

```
浏览器 (STA模式)                 HTTP Server              NVS (app_config)
────────────────────────────────────────────────────────────────────────────
GET /settings
                                 serve settings.html
                                 ← HTML 页面

GET /api/settings
                                 nvs_manager_get_str("backend_url")
                                 ← {"backend_url": "http://..."}
                                   (NVS有值用NVS，否则用CONFIG_BACKEND_URL)

POST /api/settings
  body: {"backend_url":"http://..."}
                                 nvs_manager_set_str("backend_url", url)
                                 ← 200 OK
                                 vTaskDelay(2000ms)
                                 esp_restart()

设备重启
                                                           读取 "backend_url"
image_fetcher_init()
  nvs_manager_get_str("backend_url", buf)
  有值 → 使用 NVS 值
  无值 → fallback CONFIG_BACKEND_URL
  snprintf(s_backend_url, ...)
```

---

## image_fetcher 模块变更

### 新增静态变量

```c
/* 运行时 backend URL，init 时从 NVS 加载，fallback 到 Kconfig 默认值 */
static char s_backend_url[128] = CONFIG_BACKEND_URL;
```

### `image_fetcher_init` 扩展

```c
/* 从 NVS 读取 backend_url，覆盖编译时默认值 */
char nvs_url[128] = {0};
size_t nvs_url_len = sizeof(nvs_url);
esp_err_t nvs_err = nvs_manager_get_str("backend_url", nvs_url, &nvs_url_len);
if (nvs_err == ESP_OK && strlen(nvs_url) > 0) {
    strncpy(s_backend_url, nvs_url, sizeof(s_backend_url) - 1);
    ESP_LOGI(TAG, "从 NVS 加载 backend_url: %s", s_backend_url);
} else {
    ESP_LOGI(TAG, "使用默认 backend_url: %s", s_backend_url);
}
```

### URL 构造改用静态变量

原有 `#define BACKEND_URL CONFIG_BACKEND_URL` 替换为使用 `s_backend_url`：

```c
snprintf(api_url, sizeof(api_url), "%s/v1/device/%s/next", s_backend_url, DEVICE_ID);
// rating URL 同理
snprintf(rating_url, sizeof(rating_url), "%s/v1/item/%" PRIu32 "/rating", s_backend_url, s_current_item_id);
```

---

## HTTP Server 模块变更

### 新增 handler：GET /settings

```c
static esp_err_t settings_get_handler(httpd_req_t *req) {
    return serve_static_file(req, "settings.html");
}
```

### 新增 handler：GET /api/settings

```c
static esp_err_t api_settings_get_handler(httpd_req_t *req) {
    char url[128] = CONFIG_BACKEND_URL;  /* fallback */
    size_t len = sizeof(url);
    nvs_manager_get_str("backend_url", url, &len);

    char json[160];
    int n = snprintf(json, sizeof(json), "{\"backend_url\":\"%s\"}", url);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, n);
    return ESP_OK;
}
```

### 新增 handler：POST /api/settings

```c
static esp_err_t api_settings_post_handler(httpd_req_t *req) {
    char body[160] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[ret] = '\0';

    /* 从 JSON body 解析 backend_url（简单 strstr，无需第三方库） */
    char url[128] = {0};
    const char *key = "\"backend_url\":\"";
    char *p = strstr(body, key);
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing backend_url"); return ESP_FAIL; }
    p += strlen(key);
    char *end = strchr(p, '"');
    if (!end) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "malformed json"); return ESP_FAIL; }
    size_t url_len = end - p;
    if (url_len == 0 || url_len >= sizeof(url)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid url length");
        return ESP_FAIL;
    }
    strncpy(url, p, url_len);

    nvs_manager_set_str("backend_url", url);
    ESP_LOGI(TAG, "POST /api/settings -> 保存 backend_url: %s，2秒后重启", url);

    httpd_resp_send(req, "{\"ok\":true}", -1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}
```

### URI 注册

```c
static const httpd_uri_t settings_uri = { .uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler };
static const httpd_uri_t api_settings_get_uri = { .uri = "/api/settings", .method = HTTP_GET, .handler = api_settings_get_handler };
static const httpd_uri_t api_settings_post_uri = { .uri = "/api/settings", .method = HTTP_POST, .handler = api_settings_post_handler };
```

`http_server_start()` 中在通配符 handler 之前注册以上三个 URI。同时 `config.max_uri_handlers` 从默认 8 调整为 12（默认值支撑不了新增数量）。

---

## Web 页面

### settings.html（新文件）

页面加载时 `fetch('/api/settings')` 获取当前值并填入输入框；提交时 `fetch('/api/settings', {method:'POST', body: JSON.stringify({backend_url: val})})` 发送，成功后显示"正在重启..."提示。

```
┌────────────────────────────┐
│  RSS 新闻配置              │
│                            │
│  Backend URL               │
│  [http://192.168.x.x:8080] │
│                            │
│  [ 保存并重启 ]            │
│                            │
│  ← 返回                    │
└────────────────────────────┘
```

样式复用 `styles.css` 中已有的 `.card`、`input`、`button`、`.back` 类。

### status.html 变更

在现有页面底部（返回链接之前）新增一个入口：

```html
<a class="btn btn-secondary" href="/settings">RSS 新闻配置</a>
```

---

## 关键参数

| 参数 | 值 | 说明 |
|------|----|------|
| NVS 键名 | `"backend_url"` | namespace: `app_config` |
| URL 最大长度 | 127 字节 | 含 null terminator 共 128 |
| 保存后延迟重启 | 2000ms | 与 WiFi 保存行为一致 |
| max_uri_handlers | 12 | 默认 8 不够，需显式设置 |
| fallback | `CONFIG_BACKEND_URL` | Kconfig 默认值保留 |
