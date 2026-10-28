#include "config_model.h"
#include <cJSON.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool config_model_http_url(const char *url)
{
    if (!url) return false;
    const char *host = NULL;
    if (strncmp(url, "http://", 7) == 0) host = url + 7;
    if (strncmp(url, "https://", 8) == 0) host = url + 8;
    if (!host || !*host || *host == '/' || *host == '?' || *host == '#') return false;
    const char *end = host + strcspn(host, "/?#");
    if (memchr(host, '@', (size_t)(end - host))) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p <= 32 || *p >= 127 || *p == '\\') return false;
    }
    return true;
}

static bool valid_device(const char *value)
{
    if (!*value) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return false;
    }
    return true;
}

static bool valid_headers(const char *json)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    bool valid = cJSON_IsArray(root) && cJSON_GetArraySize(root) <= 8;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(entry, "k");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "v");
        if (!cJSON_IsObject(entry) || !cJSON_IsString(key) || !cJSON_IsString(value)) { valid = false; break; }
        size_t size = strlen(key->valuestring);
        if (!size || size > 64 || strlen(value->valuestring) > 256) { valid = false; break; }
        for (const unsigned char *p = (const unsigned char *)key->valuestring; *p; p++) {
            if (!isalnum(*p) && !strchr("!#$%&'*+-.^_`|~", *p)) valid = false;
        }
        for (const unsigned char *p = (const unsigned char *)value->valuestring; *p; p++) {
            if (*p < 32 || *p == 127) valid = false;
        }
        const char *reserved[] = {"Host", "Content-Length", "Transfer-Encoding", "Connection", "Content-Type"};
        for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
            if (strcasecmp(key->valuestring, reserved[i]) == 0) valid = false;
        }
    }
    cJSON_Delete(root);
    return valid;
}

bool config_model_validate(const app_config_t *config)
{
    if (!config || config->schema_version != APP_CONFIG_SCHEMA) return false;
    /* 字段长度先验证，后续字符串操作才有明确边界 */
    if (!memchr(config->api_base_url, 0, sizeof(config->api_base_url))
        || !memchr(config->device_id, 0, sizeof(config->device_id))
        || !memchr(config->http_headers, 0, sizeof(config->http_headers))
        || !memchr(config->wifi_ssid, 0, sizeof(config->wifi_ssid))
        || !memchr(config->wifi_password, 0, sizeof(config->wifi_password))
        || !memchr(config->legacy_next_url, 0, sizeof(config->legacy_next_url))) return false;
    if (!valid_device(config->device_id) || !valid_headers(config->http_headers)) return false;
    if (config->api_base_url[0]) {
        if (!config_model_http_url(config->api_base_url) || strpbrk(config->api_base_url, "?#")) return false;
    } else if (!config->legacy_next_url[0]) return false;
    if (config->legacy_next_url[0] && !config_model_http_url(config->legacy_next_url)) return false;
    size_t pass_len = strlen(config->wifi_password);
    return !pass_len || (pass_len >= 8 && pass_len <= 63)
        || (pass_len == 64 && strspn(config->wifi_password, "0123456789abcdefABCDEF") == 64);
}

static bool field(const cJSON *root, const char *key, char *out, size_t size, bool optional)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!value && optional) { out[0] = '\0'; return true; }
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= size) return false;
    strcpy(out, value->valuestring);
    return true;
}

bool config_model_parse(const char *json, app_config_t *out)
{
    if (!json || !out) return false;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *headers = cJSON_GetObjectItemCaseSensitive(root, "headers");
    app_config_t config = {.schema_version = APP_CONFIG_SCHEMA};
    char *header_json = cJSON_PrintUnformatted(headers);
    bool valid = cJSON_IsObject(root) && cJSON_IsNumber(version) && version->valuedouble == APP_CONFIG_SCHEMA
        && field(root, "api_base_url", config.api_base_url, sizeof(config.api_base_url), false)
        && field(root, "device_id", config.device_id, sizeof(config.device_id), false)
        && field(root, "wifi_ssid", config.wifi_ssid, sizeof(config.wifi_ssid), false)
        && field(root, "wifi_password", config.wifi_password, sizeof(config.wifi_password), false)
        && field(root, "legacy_next_url", config.legacy_next_url, sizeof(config.legacy_next_url), true)
        && header_json && strlen(header_json) < sizeof(config.http_headers);
    if (valid) {
        strcpy(config.http_headers, header_json);
        valid = config_model_validate(&config);
    }
    free(header_json);
    cJSON_Delete(root);
    if (valid) *out = config;
    return valid;
}

char *config_model_json(const app_config_t *config, bool include_wifi)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *headers = cJSON_Parse(config->http_headers);
    if (!root || !headers) { cJSON_Delete(root); cJSON_Delete(headers); return NULL; }
    bool ok = cJSON_AddNumberToObject(root, "schema_version", config->schema_version)
        && cJSON_AddStringToObject(root, "api_base_url", config->api_base_url)
        && cJSON_AddStringToObject(root, "device_id", config->device_id)
        && cJSON_AddStringToObject(root, "legacy_next_url", config->legacy_next_url)
        && cJSON_AddBoolToObject(root, "migration_required", config->legacy_next_url[0] != 0);
    if (!cJSON_AddItemToObject(root, "headers", headers)) { cJSON_Delete(headers); ok = false; }
    if (include_wifi) {
        ok = ok && cJSON_AddStringToObject(root, "wifi_ssid", config->wifi_ssid)
            && cJSON_AddStringToObject(root, "wifi_password", config->wifi_password);
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}

bool config_model_migrate_url(app_config_t *config, const char *legacy_url)
{
    if (!config || !legacy_url || strlen(legacy_url) >= CONFIG_URL_MAX || !config_model_http_url(legacy_url)) return false;
    const char *path = strchr(strstr(legacy_url, "://") + 3, '/');
    const char *marker = path ? strstr(path, "/v1/device/") : NULL;
    if (marker) {
        const char *device = marker + strlen("/v1/device/");
        const char *suffix = strchr(device, '/');
        size_t device_len = suffix ? (size_t)(suffix - device) : 0;
        if (suffix && strcmp(suffix, "/next") == 0 && device_len && device_len < CONFIG_DEVICE_MAX) {
            app_config_t migrated = *config;
            size_t base_len = (size_t)(marker - legacy_url);
            memcpy(migrated.api_base_url, legacy_url, base_len);
            migrated.api_base_url[base_len] = '\0';
            memcpy(migrated.device_id, device, device_len);
            migrated.device_id[device_len] = '\0';
            migrated.legacy_next_url[0] = '\0';
            if (config_model_validate(&migrated)) { *config = migrated; return true; }
        }
    }
    if ((!path || strcmp(path, "/") == 0) && !strpbrk(legacy_url, "?#")) {
        strcpy(config->api_base_url, legacy_url);
        size_t size = strlen(config->api_base_url);
        if (config->api_base_url[size - 1] == '/') config->api_base_url[size - 1] = '\0';
        config->legacy_next_url[0] = '\0';
    } else {
        config->api_base_url[0] = '\0';
        strcpy(config->legacy_next_url, legacy_url);
    }
    return true;
}

static size_t base_length(const app_config_t *config)
{
    size_t size = strlen(config->api_base_url);
    while (size && config->api_base_url[size - 1] == '/') size--;
    return size;
}

bool config_model_default_device_id(const uint8_t mac[6], char *out, size_t size)
{
    if (!mac || !out || size < sizeof("esp32s3-000000000000")) return false;
    int n = snprintf(out, size, "esp32s3-%02x%02x%02x%02x%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return n > 0 && (size_t)n < size && valid_device(out);
}

bool config_model_next_url(const app_config_t *config, char *out, size_t size)
{
    if (!size || !config_model_validate(config)) return false;
    int n = config->legacy_next_url[0] ? snprintf(out, size, "%s", config->legacy_next_url)
        : snprintf(out, size, "%.*s/v1/device/%s/next", (int)base_length(config), config->api_base_url, config->device_id);
    return n >= 0 && (size_t)n < size;
}

bool config_model_rating_url(const app_config_t *config, uint32_t item_id, char *out, size_t size)
{
    if (!size || !item_id || !config_model_validate(config) || !config->api_base_url[0]) return false;
    int n = snprintf(out, size, "%.*s/v1/item/%" PRIu32 "/rating", (int)base_length(config), config->api_base_url, item_id);
    return n >= 0 && (size_t)n < size;
}
