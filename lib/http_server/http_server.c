/**
 * @file http_server.c
 * @brief HTTP Server 实现 - 配网页面和管理页面 (SPIFFS静态文件)
 */

#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_spiffs.h>
#include "http_server.h"
#include "wifi_manager.h"
#include "rss_reader.h"
#include "nvs_manager.h"

static const char *TAG = "HTTP_SERVER";

#define SPIFFS_BASE_PATH "/www"
#define SPIFFS_PARTITION_LABEL "storage"

static void url_decode(char *str)
{
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static const char *get_content_type(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=UTF-8";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    return "application/octet-stream";
}

static esp_err_t serve_static_file(httpd_req_t *req, const char *filename)
{
    char filepath[64];
    snprintf(filepath, sizeof(filepath), SPIFFS_BASE_PATH "/%s", filename);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(fsize);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for %s", fsize, filename);
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    fread(buf, 1, fsize, f);
    fclose(f);

    httpd_resp_set_type(req, get_content_type(filename));
    httpd_resp_send(req, buf, fsize);
    free(buf);
    return ESP_OK;
}

/**
 * @brief 根路径处理器 - 根据当前WiFi模式返回对应页面
 *
 * URI: /
 * 方法: GET
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    wifi_info_t *info = wifi_manager_get_info();

    if (info->mode == MODE_AP) {
        ESP_LOGI(TAG, "GET / -> serving index.html (AP mode)");
        return serve_static_file(req, "index.html");
    } else {
        ESP_LOGI(TAG, "GET / -> serving status.html (STA mode)");
        return serve_static_file(req, "status.html");
    }
}

/**
 * @brief 配置页面处理器 - 强制显示WiFi配置页面
 *
 * URI: /config
 * 方法: GET
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /config -> serving index.html");
    return serve_static_file(req, "index.html");
}

/**
 * @brief 保存WiFi凭证处理器 - 处理POST请求，保存凭证并重启
 *
 * URI: /save
 * 方法: POST
 */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /save -> received WiFi config submission");
    char content[512];
    char ssid[32] = {0};
    char password[64] = {0};

    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        ESP_LOGE(TAG, "POST /save -> failed to receive POST data (ret=%d)", ret);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    content[ret] = '\0';
    ESP_LOGI(TAG, "POST /save -> raw form data: %s", content);

    char *ptr = content;
    while (*ptr) {
        while (*ptr == '&') ptr++;
        if (strncmp(ptr, "ssid=", 5) == 0) {
            ptr += 5;
            char *end = strchr(ptr, '&');
            if (end) *end = '\0';
            strncpy(ssid, ptr, sizeof(ssid) - 1);
            url_decode(ssid);
            if (end) ptr = end + 1;
            else break;
        } else if (strncmp(ptr, "password=", 9) == 0) {
            ptr += 9;
            char *end = strchr(ptr, '&');
            if (end) *end = '\0';
            strncpy(password, ptr, sizeof(password) - 1);
            url_decode(password);
            break;
        } else {
            ptr++;
        }
    }

    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "POST /save -> empty SSID, rejecting");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "POST /save -> parsed: ssid=%s, password=%s", ssid, password);
    ESP_LOGI(TAG, "Saving WiFi credentials and rebooting to STA mode...");
    wifi_manager_save_credentials(ssid, password);

    serve_static_file(req, "success.html");

    // 延迟2秒后重启, 让浏览器有时间显示成功页面
    ESP_LOGI(TAG, "Rebooting to STA mode...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

/**
 * @brief 状态API处理器 - 返回JSON格式的设备状态
 *
 * URI: /api/status
 * 方法: GET
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_info_t *info = wifi_manager_get_info();

    char json[256];
    int len = snprintf(json, sizeof(json),
        "{\"status\":%d,\"mode\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"ap_ssid\":\"%s\","
        "\"heap\":%u,\"uptime\":%u}",
        info->status,
        info->mode,
        info->ip,
        info->ssid,
        info->ap_ssid,
        (unsigned int)esp_get_free_heap_size(),
        (unsigned int)(esp_timer_get_time() / 1000000)
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

/**
 * @brief 重连WiFi处理器 - 触发WiFi重新连接
 *
 * URI: /reconnect
 * 方法: GET
 */
static esp_err_t reconnect_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /reconnect -> triggering WiFi reconnect");
    serve_static_file(req, "reconnect.html");
    wifi_manager_reconnect();
    return ESP_OK;
}

/**
 * @brief 重置处理器 - 清除WiFi凭证并重启进入AP模式
 *
 * URI: /reset
 * 方法: GET
 */
static esp_err_t reset_get_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "GET /reset -> clearing credentials and rebooting to AP mode");
    serve_static_file(req, "reset.html");

    wifi_manager_clear_credentials();

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Rebooting now...");
    esp_restart();

    return ESP_OK;
}

/**
 * @brief RSS条目HTML页面处理器 - 动态生成HTML并引用SPIFFS中的CSS
 *
 * URI: /api/rss
 * 方法: GET
 */
static esp_err_t rss_item_get_handler(httpd_req_t *req)
{
    rss_cached_item_t *item = rss_reader_get_cached_item();

    char html[2048];
    int len;

    if (item->valid) {
        len = snprintf(html, sizeof(html),
            "<!DOCTYPE html>"
            "<html><head>"
            "<meta charset=\"UTF-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>RSS Item</title>"
            "<link rel=\"stylesheet\" href=\"/styles.css\">"
            "</head><body>"
            "<div class=\"card\">"
            "<h1>RSS Item</h1>"
            "<p class=\"meta\">Random from cache</p>"
            "<h2>Title</h2>"
            "<p>%s</p>"
            "<h2>Link</h2>"
            "<div class=\"link-box\"><a href=\"%s\" target=\"_blank\">%s</a></div>"
            "<h2>PubDate</h2>"
            "<p>%s</p>"
            "<a class=\"btn\" href=\"/api/rss\" onclick=\"location.reload();return false;\">Refresh</a>"
            "<div class=\"back\">"
            "<a href=\"/\">&larr; Back to Home</a>"
            "</div>"
            "</div>"
            "</body></html>",
            item->title, item->link, item->link, item->pubDate);
    } else {
        len = snprintf(html, sizeof(html),
            "<!DOCTYPE html>"
            "<html><head>"
            "<meta charset=\"UTF-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>RSS Item</title>"
            "<link rel=\"stylesheet\" href=\"/styles.css\">"
            "</head><body>"
            "<div class=\"card\">"
            "<h1>RSS Item</h1>"
            "<div class=\"no-data\">"
            "<p>No RSS item available yet.</p>"
            "<p>Please wait for the RSS reader to fetch data.</p>"
            "</div>"
            "<a class=\"btn\" href=\"/api/rss\">Refresh</a>"
            "<div class=\"back\">"
            "<a href=\"/\">&larr; Back to Home</a>"
            "</div>"
            "</div>"
            "</body></html>");
    }

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send(req, html, len);
    return ESP_OK;
}

/**
 * @brief 静态文件捕获处理器 - 服务SPIFFS中的任意静态文件
 *
 * URI: wildcard catch-all
 * 方法: GET
 */
static esp_err_t static_file_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET %s -> serving static file", req->uri);
    // URI starts with '/', skip it for filename lookup
    const char *filename = req->uri + 1;
    if (*filename == '\0') {
        filename = "index.html";
    }
    return serve_static_file(req, filename);
}

/**
 * @brief 设置页面处理器 - 返回 settings.html
 *
 * URI: /settings
 * 方法: GET
 */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /settings -> serving settings.html");
    return serve_static_file(req, "settings.html");
}

/**
 * @brief 设置 API 读取处理器 - 返回当前 backend_url（JSON）
 *
 * URI: /api/settings
 * 方法: GET
 */
static esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    char url[128];
    strncpy(url, CONFIG_BACKEND_URL, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    size_t len = sizeof(url);
    nvs_manager_get_str("backend_url", url, &len);

    char json[160];
    int n = snprintf(json, sizeof(json), "{\"backend_url\":\"%s\"}", url);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, n);
    return ESP_OK;
}

/**
 * @brief 设置 API 保存处理器 - 解析 JSON body，保存 backend_url 到 NVS，延迟重启
 *
 * URI: /api/settings
 * 方法: POST
 */
static esp_err_t api_settings_post_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        ESP_LOGE(TAG, "POST /api/settings -> 接收 body 失败 (ret=%d)", ret);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[ret] = '\0';
    ESP_LOGI(TAG, "POST /api/settings -> body: %s", body);

    /* 从 JSON body 中提取 backend_url 值（简单 strstr，无需第三方库） */
    char url[128] = {0};
    const char *key = "\"backend_url\":\"";
    char *p = strstr(body, key);
    if (!p) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing backend_url");
        return ESP_FAIL;
    }
    p += strlen(key);
    char *end = strchr(p, '"');
    if (!end) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "malformed json");
        return ESP_FAIL;
    }
    size_t url_len = (size_t)(end - p);
    if (url_len == 0 || url_len >= sizeof(url)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid url length");
        return ESP_FAIL;
    }
    strncpy(url, p, url_len);
    url[url_len] = '\0';

    nvs_manager_set_str("backend_url", url);
    ESP_LOGI(TAG, "POST /api/settings -> 保存 backend_url: %s，2秒后重启", url);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// URI路由定义 - 具体路由在前, 通配fallback在最后
static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
};

static const httpd_uri_t config_uri = {
    .uri = "/config",
    .method = HTTP_GET,
    .handler = config_get_handler,
};

static const httpd_uri_t save_uri = {
    .uri = "/save",
    .method = HTTP_POST,
    .handler = save_post_handler,
};

static const httpd_uri_t status_uri = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
};

static const httpd_uri_t reconnect_uri = {
    .uri = "/reconnect",
    .method = HTTP_GET,
    .handler = reconnect_get_handler,
};

static const httpd_uri_t reset_uri = {
    .uri = "/reset",
    .method = HTTP_GET,
    .handler = reset_get_handler,
};

static const httpd_uri_t rss_item_uri = {
    .uri = "/api/rss",
    .method = HTTP_GET,
    .handler = rss_item_get_handler,
};

static const httpd_uri_t settings_uri = {
    .uri = "/settings",
    .method = HTTP_GET,
    .handler = settings_get_handler,
};

static const httpd_uri_t api_settings_get_uri = {
    .uri = "/api/settings",
    .method = HTTP_GET,
    .handler = api_settings_get_handler,
};

static const httpd_uri_t api_settings_post_uri = {
    .uri = "/api/settings",
    .method = HTTP_POST,
    .handler = api_settings_post_handler,
};

static const httpd_uri_t static_file_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = static_file_handler,
};

/**
 * @brief 挂载SPIFFS分区
 *
 * @return esp_err_t ESP_OK on success
 */
static esp_err_t spiffs_mount_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = SPIFFS_PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition '%s' (%s)",
                 SPIFFS_PARTITION_LABEL, esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at %s: %d/%d KB used",
                 SPIFFS_BASE_PATH, (int)(used / 1024), (int)(total / 1024));
    }

    return ESP_OK;
}

/**
 * @brief 启动HTTP服务器
 *
 * 挂载SPIFFS, 注册所有URI处理器，监听80端口
 * @return HTTP服务器句柄，启动失败返回NULL
 */
httpd_handle_t http_server_start(void)
{
    spiffs_mount_storage();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server on port: %d", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers: /, /config, /save, /api/status, /api/rss, /settings, /api/settings, /reconnect, /reset, /*");
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &config_uri);
        httpd_register_uri_handler(server, &save_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &reconnect_uri);
        httpd_register_uri_handler(server, &reset_uri);
        httpd_register_uri_handler(server, &rss_item_uri);
        httpd_register_uri_handler(server, &settings_uri);
        httpd_register_uri_handler(server, &api_settings_get_uri);
        httpd_register_uri_handler(server, &api_settings_post_uri);
        httpd_register_uri_handler(server, &static_file_uri);
        ESP_LOGI(TAG, "HTTP server started successfully");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    return server;
}

/**
 * @brief 停止HTTP服务器
 *
 * @param server HTTP服务器句柄
 */
void http_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}
