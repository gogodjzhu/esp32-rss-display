#ifndef CONTENT_ITEM_H
#define CONTENT_ITEM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CONTENT_TITLE_MAX 384
#define CONTENT_TIME_MAX 32
#define CONTENT_URL_MAX 512

typedef struct {
    uint32_t id;
    char title[CONTENT_TITLE_MAX];
    char published_at[CONTENT_TIME_MAX];
    char image_url[CONTENT_URL_MAX];
} content_item_t;

/* 解析失败不改变调用方已有内容 */
bool content_item_parse(const char *json, content_item_t *out);
void content_item_banner(const content_item_t *item, char *out, size_t size);
#ifdef __cplusplus
}
#endif
#endif
