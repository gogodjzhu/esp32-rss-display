#include "http_server.h"
#include "app_config.h"
#include "app_controller.h"
#include "button_input.h"
#include "network_worker.h"
#include "wifi_manager.h"
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static httpd_handle_t s_server;
static bool s_mounted;
static const char *TAG = "HTTP";

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!json) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t receive_body(httpd_req_t *req, char *out, size_t size)
{
    if (!req->content_len || req->content_len >= size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
        return ESP_ERR_INVALID_SIZE;
    }
    size_t total = 0;
    while (total < req->content_len) {
        int count = httpd_req_recv(req, out + total, req->content_len - total);
        if (count <= 0) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "incomplete body");
            return ESP_FAIL;
        }
        total += count;
    }
    /* 表单和 JSON 都不允许内嵌 NUL */
    if (memchr(out, 0, total)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_ERR_INVALID_ARG;
    }
    out[total] = '\0';
    return ESP_OK;
}

static esp_err_t serve_file(httpd_req_t *req, const char *name)
{
    /* 页面文件只在固定资源目录内，禁止路径穿越 */
    if (strchr(name, '/') || strstr(name, "..")) return httpd_resp_send_404(req);
    char path[96];
    if (snprintf(path, sizeof(path), "/www/%s", name) >= sizeof(path)) return httpd_resp_send_404(req);
    FILE *file = fopen(path, "rb");
    if (!file) return httpd_resp_send_404(req);
    const char *ext = strrchr(name, '.');
    httpd_resp_set_type(req, ext && strcmp(ext, ".js") == 0 ? "application/javascript"
        : ext && strcmp(ext, ".css") == 0 ? "text/css" : "text/html; charset=UTF-8");
    char buffer[768];
    size_t size;
    esp_err_t err = ESP_OK;
    while ((size = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        err = httpd_resp_send_chunk(req, buffer, size);
        if (err != ESP_OK) break;
    }
    if (ferror(file)) err = ESP_FAIL;
    fclose(file);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    wifi_info_t wifi;
    wifi_manager_get_snapshot(&wifi);
    return serve_file(req, wifi.mode == MODE_AP ? "index.html" : "status.html");
}

static esp_err_t page_handler(httpd_req_t *req)
{
    return serve_file(req, req->user_ctx);
}

static esp_err_t static_handler(httpd_req_t *req)
{
    char name[64];
    size_t size = strcspn(req->uri + 1, "?");
    if (!size || size >= sizeof(name)) return httpd_resp_send_404(req);
    memcpy(name, req->uri + 1, size);
    name[size] = '\0';
    return serve_file(req, name);
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool decode_form(const char *value, char *out, size_t size)
{
    size_t position = 0;
    while (*value) {
        unsigned char ch = *value++;
        if (ch == '%') {
            if (!value[0] || !value[1]) return false;
            int high = hex_value(value[0]), low = hex_value(value[1]);
            if (high < 0 || low < 0) return false;
            ch = high * 16 + low;
            value += 2;
        } else if (ch == '+') ch = ' ';
        if (!ch || position + 1 >= size) return false;
        out[position++] = ch;
    }
    out[position] = '\0';
    return true;
}

static esp_err_t save_wifi_handler(httpd_req_t *req)
{
    char body[512], ssid[33] = "", password[65] = "";
    if (receive_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;
    char *context, *part = strtok_r(body, "&", &context);
    bool valid = true;
    while (part) {
        char *equals = strchr(part, '=');
        if (!equals) { valid = false; break; }
        *equals++ = '\0';
        if (strcmp(part, "ssid") == 0) valid = decode_form(equals, ssid, sizeof(ssid));
        else if (strcmp(part, "password") == 0) valid = decode_form(equals, password, sizeof(password));
        if (!valid) break;
        part = strtok_r(NULL, "&", &context);
    }
    if (!valid || !ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid WiFi credentials");
    esp_err_t err = app_config_save_wifi(ssid, password);
    if (err != ESP_OK) return httpd_resp_send_err(req,
        err == ESP_ERR_INVALID_ARG ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi config not saved");
    err = serve_file(req, "success.html");
    if (err == ESP_OK) app_controller_request_restart(2000);
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_info_t wifi;
    app_snapshot_t app;
    app_config_t config;
    app_config_status_t config_status;
    wifi_manager_get_snapshot(&wifi);
    app_controller_get_snapshot(&app);
    app_config_get_active(&config);
    app_config_get_status(&config_status);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(root, "device_id", config.device_id);
    cJSON_AddStringToObject(root, "api_base_url", config.api_base_url[0] ? config.api_base_url : config.legacy_next_url);
    cJSON_AddStringToObject(root, "config_state", app_config_state_name(config_status.state));
    cJSON_AddNumberToObject(root, "config_state_code", config_status.state);
    cJSON_AddBoolToObject(root, "config_degraded", app_config_is_degraded());
    if (config_status.legacy_invalid_url[0])
        cJSON_AddStringToObject(root, "legacy_invalid_url", config_status.legacy_invalid_url);
    cJSON_AddNumberToObject(root, "status", wifi.status);
    cJSON_AddNumberToObject(root, "mode", wifi.mode);
    cJSON_AddStringToObject(root, "ip", wifi.ip);
    cJSON_AddStringToObject(root, "ssid", wifi.ssid);
    cJSON_AddStringToObject(root, "ap_ssid", wifi.ap_ssid);
    cJSON_AddNumberToObject(root, "disconnect_reason", wifi.disconnect_reason);
    cJSON_AddNumberToObject(root, "retry_count", wifi.retry_count);
    cJSON_AddNumberToObject(root, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "view", app.view);
    cJSON_AddBoolToObject(root, "network_busy", app.network_busy);
    cJSON_AddNumberToObject(root, "item_id", app.item.id);
    cJSON_AddStringToObject(root, "title", app.item.title);
    cJSON_AddStringToObject(root, "published_at", app.item.published_at);
    cJSON_AddNumberToObject(root, "content_outcome", app.outcome);
    cJSON_AddStringToObject(root, "last_error", esp_err_to_name(app.last_error));
    cJSON_AddNumberToObject(root, "last_http_status", app.last_http_status);
    cJSON_AddNumberToObject(root, "failures", app.failures);
    cJSON_AddNumberToObject(root, "last_success_ms", app.last_success_ms);
    cJSON_AddNumberToObject(root, "ui_stack_free", app.ui_stack_free);
    cJSON_AddNumberToObject(root, "network_stack_free", network_worker_stack_free());
    cJSON_AddNumberToObject(root, "button_events_dropped", button_input_dropped());
    return send_json(req, root);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    app_config_t config;
    app_config_get_saved(&config);
    char *json = config_model_json(&config, false);
    if (!json) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[1536];
    if (receive_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *base = cJSON_GetObjectItemCaseSensitive(root, "api_base_url");
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *headers = cJSON_GetObjectItemCaseSensitive(root, "headers");
    char *json = cJSON_IsArray(headers) ? cJSON_PrintUnformatted(headers) : NULL;
    app_config_t config;
    app_config_get_saved(&config);
    bool valid = cJSON_IsObject(root) && cJSON_IsString(base) && cJSON_IsString(device) && json
        && strlen(base->valuestring) < sizeof(config.api_base_url)
        && strlen(device->valuestring) < sizeof(config.device_id) && strlen(json) < sizeof(config.http_headers);
    if (valid) {
        strcpy(config.api_base_url, base->valuestring);
        strcpy(config.device_id, device->valuestring);
        strcpy(config.http_headers, json);
        config.legacy_next_url[0] = '\0';
        valid = config_model_validate(&config);
    }
    free(json);
    cJSON_Delete(root);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid API config");
    esp_err_t err = app_config_save(&config);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config not saved");
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, "{\"ok\":true,\"restart_required\":true}", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) app_controller_request_restart(2000);
    return err;
}

static esp_err_t reconnect_handler(httpd_req_t *req)
{
    if (wifi_manager_reconnect() != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "reconnect unavailable");
    return serve_file(req, "reconnect.html");
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    if (app_config_clear_wifi() != ESP_OK) return httpd_resp_send_500(req);
    esp_err_t err = serve_file(req, "reset.html");
    if (err == ESP_OK) app_controller_request_restart(1000);
    return err;
}

httpd_handle_t http_server_start(void)
{
    if (s_server) return s_server;
    if (!s_mounted) {
        esp_vfs_spiffs_conf_t fs = {.base_path = "/www", .partition_label = "storage", .max_files = 3, .format_if_mount_failed = false};
        esp_err_t err = esp_vfs_spiffs_register(&fs);
        if (err != ESP_OK) { ESP_LOGE(TAG, "资源分区挂载失败: %s", esp_err_to_name(err)); return NULL; }
        s_mounted = true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &config) != ESP_OK) return NULL;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/config", .method = HTTP_GET, .handler = page_handler, .user_ctx = "index.html"},
        {.uri = "/save", .method = HTTP_POST, .handler = save_wifi_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/settings", .method = HTTP_GET, .handler = page_handler, .user_ctx = "settings.html"},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler},
        {.uri = "/reconnect", .method = HTTP_GET, .handler = reconnect_handler},
        {.uri = "/reset", .method = HTTP_GET, .handler = reset_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = static_handler},
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_server, &routes[i]) != ESP_OK) {
            httpd_stop(s_server);
            s_server = NULL;
            return NULL;
        }
    }
    return s_server;
}

void http_server_stop(httpd_handle_t server)
{
    if (server && server == s_server) { httpd_stop(server); s_server = NULL; }
}
