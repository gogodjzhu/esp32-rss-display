/**
 * @file http_server.c
 * @brief HTTP Server 实现 - 配网页面和管理页面
 */

#include <string.h>
#include <sys/param.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "http_server.h"
#include "wifi_manager.h"

static const char *TAG = "HTTP_SERVER";

// HTML页面模板 - 公共头部
#define HTML_HEADER "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>ESP32 WiFi Config</title>"
// HTML页面模板 - 样式
#define HTML_STYLE "<style>body{font-family:Arial,sans-serif;max-width:600px;margin:50px auto;padding:20px;background:#f5f5f5} .card{background:#fff;border-radius:8px;padding:30px;box-shadow:0 2px 10px rgba(0,0,0,.1)} h1{color:#333;text-align:center} h2{color:#666;margin-top:0} label{display:block;margin:15px 0 5px;color:#555} input{width:100%;padding:12px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:16px} button{width:100%;padding:15px;margin-top:20px;background:#007bff;color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer} button:hover{background:#0056b3} .btn-secondary{background:#6c757d}.btn-secondary:hover{background:#5a6268} .btn-danger{background:#dc3545}.btn-danger:hover{background:#c82333} .status{margin-top:20px;padding:15px;border-radius:4px;text-align:center} .status-connected{background:#d4edda;color:#155724} .status-disconnected{background:#f8d7da;color:#721c24} .status-ap{background:#cce5ff;color:#004085} .info{margin-top:20px;padding:15px;background:#e9ecef;border-radius:4px} .info p{margin:8px 0}</style>"
// HTML页面模板 - 脚本开始
#define HTML_SCRIPT "</head><body><div class=\"card\">"
// HTML页面模板 - 结束
#define HTML_END "</div></body></html>"

// WiFi配置页面HTML - AP模式下显示
static const char *config_page_html = 
    HTML_HEADER HTML_STYLE HTML_SCRIPT
    "<h1>WiFi Configuration</h1>"
    "<h2>Configure your WiFi Network</h2>"
    "<form method=\"POST\" action=\"/save\">"
    "<label>SSID (WiFi Name)</label>"
    "<input type=\"text\" name=\"ssid\" required placeholder=\"Enter WiFi SSID\">"
    "<label>Password</label>"
    "<input type=\"password\" name=\"password\" placeholder=\"Enter WiFi Password\">"
    "<button type=\"submit\">Save & Connect</button>"
    "</form>"
    HTML_END;

// 状态页面HTML - STA模式下显示, 包含实时状态更新
static const char *status_page_html = 
    HTML_HEADER HTML_STYLE HTML_SCRIPT
    "<h1>ESP32 Status</h1>"
    "<div id=\"status\"></div>"
    "<div class=\"info\">"
    "<p><strong>Device IP:</strong> <span id=\"ip\"></span></p>"
    "<p><strong>SSID:</strong> <span id=\"ssid\"></span></p>"
    "<p><strong>Mode:</strong> <span id=\"mode\"></span></p>"
    "<p><strong>Free Heap:</strong> <span id=\"heap\"></span></p>"
    "<p><strong>Uptime:</strong> <span id=\"uptime\"></span></p>"
    "</div>"
    "<button class=\"btn-secondary\" onclick=\"location.href='/config'\">Configure WiFi</button>"
    "<button class=\"btn-secondary\" onclick=\"location.href='/reconnect'\">Reconnect</button>"
    "<button class=\"btn-danger\" onclick=\"location.href='/reset'\">Reset to AP Mode</button>"
    "<script>"
    "function updateStatus(){"
    "fetch('/api/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('ip').innerText=d.ip||'N/A';"
    "document.getElementById('ssid').innerText=d.ssid||'N/A';"
    "document.getElementById('mode').innerText=d.mode==0?'STA':'AP';"
    "document.getElementById('heap').innerText=d.heap+' bytes';"
    "document.getElementById('uptime').innerText=d.uptime+'s';"
    "var s=document.getElementById('status');"
    "if(d.status==2)s.className='status status-connected',s.innerText='Connected';"
    "else if(d.status==4)s.className='status status-ap',s.innerText='AP Mode';"
    "else s.className='status status-disconnected',s.innerText='Disconnected';"
    "}).catch(()=>{});"
    "}"
    "setInterval(updateStatus,2000);"
    "updateStatus();"
    "</script>"
    HTML_END;

// 保存成功页面HTML
static const char *success_page_html = 
    HTML_HEADER HTML_STYLE HTML_SCRIPT
    "<h1>WiFi Saved!</h1>"
    "<div class=\"status status-connected\">Connecting to WiFi...</div>"
    "<p>Device will restart and connect to your WiFi network.</p>"
    "<script>setTimeout(()=>location.href='/',5000);</script>"
    HTML_END;

// 根路径处理函数 - 根据当前模式返回对应页面
// AP模式返回配置页面, STA模式返回状态页面
static esp_err_t root_get_handler(httpd_req_t *req)
{
    wifi_info_t *info = wifi_manager_get_info();
    
    if (info->mode == MODE_AP) {
        ESP_LOGI(TAG, "GET / -> serving config page (AP mode)");
        httpd_resp_send(req, config_page_html, HTTPD_RESP_USE_STRLEN);
    } else {
        ESP_LOGI(TAG, "GET / -> serving status page (STA mode)");
        httpd_resp_send(req, status_page_html, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// 配置页面处理函数 - 强制显示WiFi配置页面
static esp_err_t config_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /config -> serving config page");
    httpd_resp_send(req, config_page_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 保存WiFi凭证处理函数 - 处理POST请求
// 解析表单数据, 保存到NVS, 重启设备
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
    
    // 解析URL编码的表单数据
    char *ptr = content;
    while (*ptr) {
        while (*ptr == '&') ptr++;
        if (strncmp(ptr, "ssid=", 5) == 0) {
            ptr += 5;
            char *end = strchr(ptr, '&');
            if (end) *end = '\0';
            strncpy(ssid, ptr, sizeof(ssid) - 1);
            if (end) ptr = end + 1;
            else break;
        } else if (strncmp(ptr, "password=", 9) == 0) {
            ptr += 9;
            char *end = strchr(ptr, '&');
            if (end) *end = '\0';
            strncpy(password, ptr, sizeof(password) - 1);
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
    
    // 返回成功页面
    httpd_resp_send(req, success_page_html, HTTPD_RESP_USE_STRLEN);
    
    // 延迟2秒后重启, 让浏览器有时间显示成功页面
    ESP_LOGI(TAG, "Rebooting to STA mode...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    
    return ESP_OK;
}

// 状态API处理函数 - 返回JSON格式的设备状态
// 供前端页面每2秒轮询更新状态
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

// 重连处理函数 - 触发WiFi重连
static esp_err_t reconnect_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /reconnect -> triggering WiFi reconnect");
    const char *html = 
        HTML_HEADER HTML_STYLE HTML_SCRIPT
        "<h1>Reconnecting...</h1>"
        "<div class=\"status status-disconnected\">Attempting to reconnect...</div>"
        "<script>setTimeout(()=>location.href='/',5000);</script>"
        HTML_END;
    
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    
    wifi_manager_reconnect();
    
    return ESP_OK;
}

static esp_err_t reset_get_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "GET /reset -> clearing credentials and rebooting to AP mode");
    const char *html = 
        HTML_HEADER HTML_STYLE HTML_SCRIPT
        "<h1>Resetting...</h1>"
        "<div class=\"status status-ap\">Switching to AP mode...</div>"
        "<script>setTimeout(()=>location.href='/',5000);</script>"
        HTML_END;
    
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    
    wifi_manager_clear_credentials();
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Rebooting now...");
    esp_restart();
    
    return ESP_OK;
}

// URI路由定义
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

// 启动HTTP服务器
// 注册所有URI处理器, 开始监听80端口
httpd_handle_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server on port: %d", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers: /, /config, /save, /api/status, /reconnect, /reset");
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &config_uri);
        httpd_register_uri_handler(server, &save_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &reconnect_uri);
        httpd_register_uri_handler(server, &reset_uri);
        ESP_LOGI(TAG, "HTTP server started successfully");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    return server;
}

// 停止HTTP服务器
void http_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}
