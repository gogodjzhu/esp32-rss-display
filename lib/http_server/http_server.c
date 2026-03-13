/**
 * @file http_server.c
 * @brief HTTP Server 实现
 */

#include <esp_log.h>
#include <esp_http_server.h>
#include "http_server.h"

static const char *TAG = "HTTP_SERVER";

// GET / 处理器 - 返回 "Hello World"
static esp_err_t hello_get_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "Hello World", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// URI路由定义
static const httpd_uri_t hello_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = hello_get_handler,
};

httpd_handle_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server on port: %d", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &hello_uri);
    }

    return server;
}

void http_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}
