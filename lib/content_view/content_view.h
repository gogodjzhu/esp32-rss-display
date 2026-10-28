#ifndef CONTENT_VIEW_H
#define CONTENT_VIEW_H
#include "content_item.h"
#include "http_transport.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 同步渲染，由 UI 任务调用；JPEG 数据仍由调用方释放 */
esp_err_t content_view_show(const content_item_t *item, const http_blob_t *jpeg);
void content_view_tick(void);
#ifdef __cplusplus
}
#endif
#endif
