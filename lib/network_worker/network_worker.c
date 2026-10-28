#include "network_worker.h"
#include "app_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdlib.h>

static QueueHandle_t s_jobs, s_results;
static TaskHandle_t s_task;

static void worker(void *arg)
{
    (void)arg;
    app_config_t config;
    app_config_get_active(&config);
    network_job_t job;
    while (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
        network_result_t result = {.job = job};
        if (job.kind == NETWORK_NEXT) {
            result.api = backend_client_next(&config, &result.item);
            if (result.api.error == ESP_OK && result.item.image_url[0]) {
                /* 图片来自外部主机时也不携带 API 凭证 */
                result.image = http_transport_get(result.item.image_url, NULL, 32768, 15000, &result.jpeg);
            }
        } else result.api = backend_client_rate(&config, job.item_id, job.rating);
        /* UI 持续消费结果，排队期间所有权仍留在 worker */
        if (xQueueSend(s_results, &result, portMAX_DELAY) != pdTRUE) free(result.jpeg.data);
    }
}

esp_err_t network_worker_start(void)
{
    if (s_task) return ESP_OK;
    s_jobs = xQueueCreate(2, sizeof(network_job_t));
    s_results = xQueueCreate(2, sizeof(network_result_t));
    if (!s_jobs || !s_results || xTaskCreate(worker, "network", 8192, NULL, 4, &s_task) != pdPASS) {
        if (s_jobs) vQueueDelete(s_jobs);
        if (s_results) vQueueDelete(s_results);
        s_jobs = s_results = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool network_worker_submit(const network_job_t *job)
{
    return s_jobs && xQueueSend(s_jobs, job, 0) == pdTRUE;
}

bool network_worker_receive(network_result_t *out)
{
    return s_results && xQueueReceive(s_results, out, 0) == pdTRUE;
}

unsigned network_worker_stack_free(void)
{
    return s_task ? uxTaskGetStackHighWaterMark(s_task) : 0;
}
