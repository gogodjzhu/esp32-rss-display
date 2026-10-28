#include "http_transport.h"
#include "backend_client.h"
#include <esp_http_client.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 可控的分片响应与故障注入，运行实际传输及协议实现 */
struct host_client { int unused; };
static struct host_client client;
static const char *body;
static size_t body_size, position;
static int64_t declared;
static int status, closes, cleanups, credentials;
static bool complete, fail_init, fail_read;
static esp_err_t open_error;
static char last_url[512], post_body[256];

static void response(const char *data, size_t size, int64_t length, int code, bool finished)
{
    body = data;
    body_size = size;
    declared = length;
    status = code;
    position = 0;
    complete = finished;
    closes = cleanups = credentials = 0;
    fail_init = fail_read = false;
    open_error = ESP_OK;
    post_body[0] = 0;
}

esp_err_t esp_crt_bundle_attach(void *p) { (void)p; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config->disable_auto_redirect && config->crt_bundle_attach && config->timeout_ms > 0);
    snprintf(last_url, sizeof(last_url), "%s", config->url);
    return fail_init ? NULL : &client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    assert(c == &client && value);
    if (!strcmp(key, "Authorization")) credentials++;
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n) { (void)c; assert(n == 0); return open_error; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return declared; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return status; }
int esp_http_client_read(esp_http_client_handle_t c, char *out, int size)
{
    (void)c;
    if (fail_read) return -1;
    size_t count = body_size - position;
    if (count > (size_t)size) count = size;
    if (count > 3) count = 3;
    memcpy(out, body + position, count);
    position += count;
    return count;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    (void)c;
    return position == body_size && complete;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; closes++; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; cleanups++; return ESP_OK; }
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, int method) { (void)c; assert(method == HTTP_METHOD_POST); return ESP_OK; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *data, int size)
{
    (void)c;
    assert(size > 0 && (size_t)size < sizeof(post_body));
    memcpy(post_body, data, size);
    post_body[size] = 0;
    return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { (void)c; return open_error; }

static void expect_failure(size_t limit, esp_err_t error)
{
    http_blob_t blob = {.data = (void *)1, .size = 123};
    net_result_t result = http_transport_get("http://host/image.jpg", NULL, limit, 100, &blob);
    assert(result.error == error && !blob.data && !blob.size);
    assert(closes == 1 && cleanups == 1);
}

int main(void)
{
    http_blob_t blob;
    response("abcdefgh", 8, 8, 200, true);
    assert(http_transport_get("http://host/a", NULL, 8, 100, &blob).error == ESP_OK);
    assert(blob.size == 8 && !memcmp(blob.data, "abcdefgh\0", 9));
    assert(closes == 1 && cleanups == 1 && !credentials);
    free(blob.data);
    /* 未知长度、刚好达到上限、溢出、缺失分块结束与中途断流 */
    response("abcdefgh", 8, 0, 200, true);
    assert(http_transport_get("http://host/a", NULL, 8, 100, &blob).error == ESP_OK);
    free(blob.data);
    response("abcdefghi", 9, 0, 200, true);
    expect_failure(8, ESP_ERR_INVALID_SIZE);
    response("abcdefgh", 8, 9, 200, true);
    expect_failure(16, ESP_ERR_INVALID_RESPONSE);
    response("abcdefgh", 8, 0, 200, false);
    expect_failure(16, ESP_ERR_INVALID_RESPONSE);
    response("abcdefgh", 8, 9, 200, true);
    expect_failure(8, ESP_ERR_INVALID_SIZE);
    response("", 0, 0, 200, true);
    expect_failure(8, ESP_ERR_INVALID_RESPONSE);
    response("data", 4, 4, 503, true);
    expect_failure(8, ESP_ERR_INVALID_RESPONSE);
    response("data", 4, 4, 302, true);
    expect_failure(8, ESP_ERR_INVALID_RESPONSE);
    response("data", 4, 4, 200, true);
    open_error = ESP_FAIL;
    expect_failure(8, ESP_FAIL);
    response("data", 4, 4, 200, true);
    fail_read = true;
    expect_failure(8, ESP_FAIL);
    response("data", 4, 4, 200, true);
    fail_init = true;
    assert(http_transport_get("http://host/a", NULL, 8, 100, &blob).error == ESP_ERR_NO_MEM);
    assert(!cleanups && !blob.data);

    app_config_t config = {.schema_version = APP_CONFIG_SCHEMA, .api_base_url = "http://backend",
        .device_id = "device-01", .http_headers = "[{\"k\":\"Authorization\",\"v\":\"Bearer example\"}]"};
    const char *json = "{\"item_id\":42,\"title\":\"hello\"}";
    response(json, strlen(json), strlen(json), 200, true);
    content_item_t item;
    assert(backend_client_next(&config, &item).error == ESP_OK && item.id == 42);
    assert(!strcmp(last_url, "http://backend/v1/device/device-01/next") && credentials == 1);
    const char embedded[] = "{\"title\":\"x\"}\0trailing";
    response(embedded, sizeof(embedded) - 1, sizeof(embedded) - 1, 200, true);
    assert(backend_client_next(&config, &item).error == ESP_ERR_INVALID_RESPONSE);
    response("", 0, 0, 201, true);
    assert(backend_client_rate(&config, 42, 5).error == ESP_OK);
    assert(!strcmp(last_url, "http://backend/v1/item/42/rating") && credentials == 1 && cleanups == 1);
    assert(strstr(post_body, "\"rating\":5") && strstr(post_body, "\"device_id\":\"device-01\""));
    response("", 0, 0, 500, true);
    assert(backend_client_rate(&config, 42, 5).error == ESP_ERR_INVALID_RESPONSE && cleanups == 1);
    response("", 0, 0, 200, true);
    assert(backend_client_rate(&config, 0, 5).error == ESP_ERR_INVALID_ARG && !cleanups);
    puts("PASS: fragmented transport, bounds, truncation, failures, credentials, backend contracts");
    return 0;
}
