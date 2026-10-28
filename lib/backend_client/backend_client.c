#include "backend_client.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

net_result_t backend_client_next(const app_config_t *config, content_item_t *out)
{
    char url[CONTENT_URL_MAX];
    net_result_t result = {.error = ESP_ERR_INVALID_ARG};
    if (!config_model_next_url(config, url, sizeof(url))) return result;
    http_blob_t body;
    result = http_transport_get(url, config, 4096, 10000, &body);
    if (result.error != ESP_OK) return result;
    if (memchr(body.data, 0, body.size) || !content_item_parse((const char *)body.data, out))
        result.error = ESP_ERR_INVALID_RESPONSE;
    free(body.data);
    return result;
}

net_result_t backend_client_rate(const app_config_t *config, uint32_t item_id, int rating)
{
    char url[CONTENT_URL_MAX];
    net_result_t result = {.error = ESP_ERR_INVALID_ARG};
    if (rating < 1 || rating > 5 || !config_model_rating_url(config, item_id, url, sizeof(url))) return result;
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "rating", rating)
        || !cJSON_AddStringToObject(root, "device_id", config->device_id)) {
        cJSON_Delete(root);
        result.error = ESP_ERR_NO_MEM;
        return result;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) { result.error = ESP_ERR_NO_MEM; return result; }
    result = http_transport_post(url, config, json);
    free(json);
    return result;
}
