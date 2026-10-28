#ifndef HOST_ESP_HTTP_CLIENT_H
#define HOST_ESP_HTTP_CLIENT_H
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct host_client *esp_http_client_handle_t;
typedef struct {
    const char *url;
    int timeout_ms;
    int buffer_size;
    esp_err_t (*crt_bundle_attach)(void *);
    bool disable_auto_redirect;
} esp_http_client_config_t;
#define HTTP_METHOD_POST 1
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t, int);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
#endif
