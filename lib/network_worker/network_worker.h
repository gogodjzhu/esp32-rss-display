#ifndef NETWORK_WORKER_H
#define NETWORK_WORKER_H
#include "backend_client.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NETWORK_NEXT, NETWORK_RATE } network_job_kind_t;
typedef struct {
    network_job_kind_t kind;
    uint32_t request_id;
    uint32_t item_id;
    int rating;
} network_job_t;
typedef struct {
    network_job_t job;
    net_result_t api;
    net_result_t image;
    content_item_t item;
    http_blob_t jpeg;
} network_result_t;
esp_err_t network_worker_start(void);
bool network_worker_submit(const network_job_t *job);
/* 接收成功即移交 jpeg.data 所有权，调用方必须释放 */
bool network_worker_receive(network_result_t *out);
unsigned network_worker_stack_free(void);
#ifdef __cplusplus
}
#endif
#endif
