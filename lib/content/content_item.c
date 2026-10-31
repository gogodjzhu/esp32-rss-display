#include "content_item.h"
#include "config_model.h"
#include "text_utf8.h"
#include <cJSON.h>
#include <math.h>
#include <string.h>

static bool copy_field(const cJSON *root, const char *name, char *out, size_t size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value || cJSON_IsNull(value)) { out[0] = '\0'; return true; }
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= size) return false;
    strcpy(out, value->valuestring);
    return true;
}

static bool parse_stock(const cJSON *entry, content_stock_t *out)
{
    if (!cJSON_IsObject(entry)) return false;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
    const cJSON *pct  = cJSON_GetObjectItemCaseSensitive(entry, "change_pct");
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0') return false;
    if (!cJSON_IsNumber(pct) || !isfinite(pct->valuedouble)) return false;

    /* 名称按完整码点做安全截断，保留原始内容；显示裁剪交给 banner 按像素宽度处理 */
    text_utf8_copy(name->valuestring, out->name, sizeof(out->name));
    if (out->name[0] == '\0') return false;

    /* 后端用百分比浮点数传涨跌，固件转成 0.01% 的整数，避免浮点显示误差 */
    double bp = pct->valuedouble * 100.0;
    if (!isfinite(bp) || bp < (double)INT32_MIN || bp > (double)INT32_MAX) return false;

    out->change_bp = (int32_t)llround(bp);
    return true;
}

/* 行情是可选装饰字段：非法条目逐条丢弃、超出上限直接截断，
 * 任何情况都不能让标题和图片一起解析失败。 */
static void parse_stocks(const cJSON *root, content_item_t *item)
{
    const cJSON *stocks = cJSON_GetObjectItemCaseSensitive(root, "stocks");
    if (!cJSON_IsArray(stocks)) return;

    int count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, stocks) {
        if (count >= CONTENT_STOCK_MAX) break;
        if (!parse_stock(entry, &item->stocks[count])) continue;
        count++;
    }
    item->stock_count = (uint8_t)count;
}

/* 新鲜度是可选字段：缺失或非法时按默认值（0/未知）处理，不影响内容本身 */
static void parse_freshness(const cJSON *root, content_item_t *item)
{
    const cJSON *age = cJSON_GetObjectItemCaseSensitive(root, "age_sec");
    const cJSON *oc  = cJSON_GetObjectItemCaseSensitive(root, "open_count");
    if (cJSON_IsNumber(age) && isfinite(age->valuedouble)) {
        double v = age->valuedouble;
        if (v < 0) {
            item->age_sec = 0;
        } else if (v > (double)INT64_MAX) {
            item->age_sec = INT64_MAX;
        } else {
            item->age_sec = (int64_t)v;
        }
        item->freshness_known = true;
    }
    if (cJSON_IsNumber(oc) && isfinite(oc->valuedouble) && oc->valuedouble > 0) {
        item->open_count = oc->valuedouble >= (double)UINT8_MAX
            ? UINT8_MAX : (uint8_t)oc->valuedouble;
        item->freshness_known = true;
    }
    /* 两个字段都缺失时 freshness_known 保持 false */
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
        if (valid) parse_stocks(root, &item);
        if (valid) parse_freshness(root, &item);
    }
    cJSON_Delete(root);
    if (valid) *out = item;
    return valid;
}
