#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H
#include "app_state.h"
#include "content_item.h"
#include <esp_err.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { CONTENT_NONE, CONTENT_IMAGE, CONTENT_NO_IMAGE, CONTENT_DOWNLOAD_FALLBACK, CONTENT_DECODE_FALLBACK } content_outcome_t;
typedef struct {
    app_view_state_t view;
    content_item_t item;
    bool network_busy;
    content_outcome_t outcome;
    esp_err_t last_error;
    int last_http_status;
    unsigned failures;
    int64_t last_success_ms;
    unsigned ui_stack_free;
} app_snapshot_t;
/* 在调用任务中运行，拥有全部 UI 和当前内容 */
esp_err_t app_controller_run(void);
void app_controller_get_snapshot(app_snapshot_t *out);
void app_controller_request_restart(unsigned delay_ms);
#ifdef __cplusplus
}
#endif
#endif
