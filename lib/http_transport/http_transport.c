#include "http_transport.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

static esp_err_t apply_headers(esp_http_client_handle_t client, const app_config_t *config)
{
    if (!config) return ESP_OK;
    cJSON *root = cJSON_Parse(config->http_headers);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    esp_err_t err = ESP_OK;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, root) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(entry, "k");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "v");
        if (!cJSON_IsString(key) || !cJSON_IsString(value)) { err = ESP_ERR_INVALID_ARG; break; }
        err = esp_http_client_set_header(client, key->valuestring, value->valuestring);
        if (err != ESP_OK) break;
    }
    cJSON_Delete(root);
    return err;
}

static esp_http_client_handle_t create_client(const char *url, int timeout_ms)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* API 凭证不会经重定向发送给另一主机 */
        .disable_auto_redirect = true,
    };
    return esp_http_client_init(&config);
}

net_result_t http_transport_get(const char *url, const app_config_t *api_config, size_t limit,
                                int timeout_ms, http_blob_t *out)
{
    *out = (http_blob_t){0};
    net_result_t result = {.error = ESP_FAIL};
    if (!config_model_http_url(url) || !limit) { result.error = ESP_ERR_INVALID_ARG; return result; }
    esp_http_client_handle_t client = create_client(url, timeout_ms);
    if (!client) { result.error = ESP_ERR_NO_MEM; return result; }
    uint8_t *buffer = NULL;
    result.error = apply_headers(client, api_config);
    if (result.error != ESP_OK) goto cleanup;
    result.error = esp_http_client_open(client, 0);
    if (result.error != ESP_OK) goto cleanup;
    int64_t length = esp_http_client_fetch_headers(client);
    result.http_status = esp_http_client_get_status_code(client);
    if (length < 0) { result.error = ESP_FAIL; goto cleanup; }
    if (result.http_status != 200) { result.error = ESP_ERR_INVALID_RESPONSE; goto cleanup; }
    if (length > (int64_t)limit) { result.error = ESP_ERR_INVALID_SIZE; goto cleanup; }
    size_t capacity = length > 0 ? (size_t)length : limit;
    buffer = malloc(capacity + 1);
    if (!buffer) { result.error = ESP_ERR_NO_MEM; goto cleanup; }
    size_t total = 0;
    while (total < capacity) {
        int n = esp_http_client_read(client, (char *)buffer + total, capacity - total);
        if (n < 0) { result.error = ESP_FAIL; goto cleanup; }
        if (!n) break;
        total += n;
    }
    if (!esp_http_client_is_complete_data_received(client)) {
        char extra;
        int n = esp_http_client_read(client, &extra, 1);
        if (n != 0 || !esp_http_client_is_complete_data_received(client)) {
            result.error = n > 0 ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }
    if (!total || (length > 0 && total != (size_t)length)) { result.error = ESP_ERR_INVALID_RESPONSE; goto cleanup; }
    buffer[total] = '\0';
    out->data = buffer;
    out->size = total;
    buffer = NULL;
    result.error = ESP_OK;
cleanup:
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

net_result_t http_transport_post(const char *url, const app_config_t *config, const char *json)
{
    net_result_t result = {.error = ESP_ERR_NO_MEM};
    esp_http_client_handle_t client = create_client(url, 5000);
    if (!client) return result;
    result.error = apply_headers(client, config);
    if (result.error == ESP_OK) result.error = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (result.error == ESP_OK) result.error = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (result.error == ESP_OK) result.error = esp_http_client_set_post_field(client, json, strlen(json));
    if (result.error == ESP_OK) result.error = esp_http_client_perform(client);
    result.http_status = esp_http_client_get_status_code(client);
    if (result.error == ESP_OK && (result.http_status < 200 || result.http_status >= 300)) result.error = ESP_ERR_INVALID_RESPONSE;
    esp_http_client_cleanup(client);
    return result;
}
