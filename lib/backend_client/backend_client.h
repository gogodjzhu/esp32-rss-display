#ifndef BACKEND_CLIENT_H
#define BACKEND_CLIENT_H
#include "content_item.h"
#include "http_transport.h"
#ifdef __cplusplus
extern "C" {
#endif
net_result_t backend_client_next(const app_config_t *config, content_item_t *out);
net_result_t backend_client_rate(const app_config_t *config, uint32_t item_id, int rating);
#ifdef __cplusplus
}
#endif
#endif
