/**
 * @file cjk_font.c
 * @brief 中文点阵字体（Tiny TTF）实现
 */

#include <string.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_log.h>

#include "lv_conf.h"
#include "lvgl.h"
#include "cjk_font.h"

static const char *TAG = "CJK_FONT";

/* 自定义 data 分区：ESP-IDF 保留 0x00-0x3F，0x40-0xFE 供应用自由使用 */
#define FONT_PART_SUBTYPE 0x40
#define FONT_PART_LABEL   "font"

/* 默认正文字号（像素） */
#define CJK_FONT_DEFAULT_SIZE 18
/* 单字号字形缓存条目数 */
#define CJK_FONT_CACHE_CNT    160
/* 按字号缓存的上限，避免 ArrayList 化 */
#define CJK_FONT_MAX_SIZES    6

typedef struct {
    int         size_px;
    lv_font_t  *font;
} cjk_font_slot_t;

static const void                 *s_font_data   = NULL;
static size_t                      s_font_size   = 0;
static esp_partition_mmap_handle_t s_mmap_handle = 0;
static cjk_font_slot_t             s_slots[CJK_FONT_MAX_SIZES];
static int                         s_slot_count  = 0;

esp_err_t cjk_font_init(void)
{
    if (s_slot_count) return ESP_OK;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)FONT_PART_SUBTYPE, FONT_PART_LABEL);

    if (part == NULL) {
        ESP_LOGW(TAG, "未找到 font 分区（label=%s, subtype=0x%02x），中文将不可用",
                 FONT_PART_LABEL, FONT_PART_SUBTYPE);
        return ESP_ERR_NOT_FOUND;
    }

    const void *ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                       &ptr, &s_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap font 分区失败: %s", esp_err_to_name(err));
        return err;
    }

    s_font_data = ptr;
    s_font_size = part->size;
    ESP_LOGI(TAG, "中文字体分区已映射: %u bytes @ %p", (unsigned)part->size, ptr);

    /* 空白或错误资源不能交给字体解析器 */
    const uint8_t *signature = ptr;
    if ((memcmp(signature, "\x00\x01\x00\x00", 4) != 0 && memcmp(signature, "OTTO", 4) != 0)
        || cjk_font_get(CJK_FONT_DEFAULT_SIZE) == NULL) {
        ESP_LOGW(TAG, "font 分区内容不可用，标题回退默认字体（中文覆盖不完整）");
        esp_partition_munmap(s_mmap_handle);
        s_font_data = NULL;
        s_font_size = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool cjk_font_available(void)
{
    return s_font_data != NULL;
}

const lv_font_t *cjk_font_get(int size_px)
{
    if (s_font_data == NULL) {
        return NULL;
    }
    if (size_px <= 0) {
        size_px = CJK_FONT_DEFAULT_SIZE;
    }

    /* 命中已有字号 */
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].size_px == size_px) {
            return s_slots[i].font;
        }
    }

    if (s_slot_count >= CJK_FONT_MAX_SIZES) {
        ESP_LOGW(TAG, "字号缓存已满，回退到 %dpx", s_slots[0].size_px);
        return s_slots[0].font;
    }

    lv_font_t *font = lv_tiny_ttf_create_data_ex(s_font_data, s_font_size, size_px,
                                                 LV_FONT_KERNING_NONE, CJK_FONT_CACHE_CNT);
    if (font == NULL) {
        ESP_LOGE(TAG, "创建 %dpx 中文字体失败（可用堆 %d）",
                 size_px, (int)esp_get_free_heap_size());
        return NULL;
    }

    s_slots[s_slot_count].size_px = size_px;
    s_slots[s_slot_count].font    = font;
    s_slot_count++;

    ESP_LOGI(TAG, "创建中文字体 %dpx（可用堆 %d）", size_px, (int)esp_get_free_heap_size());
    return font;
}
