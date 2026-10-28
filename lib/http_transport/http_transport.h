#ifndef HTTP_TRANSPORT_H
#define HTTP_TRANSPORT_H
#include "config_model.h"
#include <esp_err.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint8_t *data; size_t size; } http_blob_t;
typedef struct { esp_err_t error; int http_status; } net_result_t;
/* 成功时数据归调用方所有，使用 free 释放；失败时返回空 buffer */
net_result_t http_transport_get(const char *url, const app_config_t *api_config, size_t limit,
                                int timeout_ms, http_blob_t *out);
net_result_t http_transport_post(const char *url, const app_config_t *config, const char *json);
#ifdef __cplusplus
}
#endif
#endif
