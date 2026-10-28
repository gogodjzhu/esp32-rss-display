#include "content_item.h"
#include "config_model.h"
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool copy_field(const cJSON *root, const char *name, char *out, size_t size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value || cJSON_IsNull(value)) { out[0] = '\0'; return true; }
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= size) return false;
    strcpy(out, value->valuestring);
    return true;
}

bool content_item_parse(const char *json, content_item_t *out)
{
    if (!json || !out) return false;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "item_id");
    content_item_t item = {0};
    /* 兼容未提供评分标识的后端；此时内容可展示，但不能评分 */
    bool valid = cJSON_IsObject(root) && (!id || (cJSON_IsNumber(id) && isfinite(id->valuedouble)
        && id->valuedouble >= 1 && id->valuedouble <= UINT32_MAX
        && floor(id->valuedouble) == id->valuedouble));
    if (valid) {
        item.id = id ? (uint32_t)id->valuedouble : 0;
        valid = copy_field(root, "title", item.title, sizeof(item.title))
            && copy_field(root, "published_at", item.published_at, sizeof(item.published_at))
            && copy_field(root, "image_url", item.image_url, sizeof(item.image_url))
            && (!item.image_url[0] || config_model_http_url(item.image_url));
    }
    cJSON_Delete(root);
    if (valid) *out = item;
    return valid;
}

void content_item_banner(const content_item_t *item, char *out, size_t size)
{
    snprintf(out, size, "%s%s%s", item->published_at,
             item->published_at[0] && item->title[0] ? "\n" : "", item->title);
}
