#include "app_controller.h"
#include "app_config.h"
#include "button_input.h"
#include "content_view.h"
#include "http_server.h"
#include "network_worker.h"
#include "ui_animation.h"
#include "wifi_manager.h"
#include "sdkconfig.h"
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "APP";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_snapshot_t s_snapshot;
static int64_t s_restart_at;
static struct {
    app_state_t state;
    network_result_t candidate;
    bool has_candidate, has_content, next_pending, online;
    uint32_t sequence, next_id, rating_id;
    int64_t retry_at, shown_at;
    wifi_status_t last_wifi;
    wifi_work_mode_t last_mode;
} s_app;

void app_controller_get_snapshot(app_snapshot_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

void app_controller_request_restart(unsigned delay_ms)
{
    int64_t deadline = esp_timer_get_time() / 1000 + delay_ms;
    portENTER_CRITICAL(&s_lock);
    if (!s_restart_at || deadline < s_restart_at) s_restart_at = deadline;
    portEXIT_CRITICAL(&s_lock);
}

static void record_error(net_result_t result)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.last_error = result.error;
    s_snapshot.last_http_status = result.http_status;
    if (result.error != ESP_OK) s_snapshot.failures++;
    portEXIT_CRITICAL(&s_lock);
    if (result.error != ESP_OK) ESP_LOGW(TAG, "请求失败: %s，HTTP=%d", esp_err_to_name(result.error), result.http_status);
}

static void request_next(int64_t now)
{
    s_app.state.next_requested = true;
    if (!s_app.online || s_app.next_pending || now < s_app.retry_at) return;
    network_job_t job = {.kind = NETWORK_NEXT, .request_id = ++s_app.sequence};
    if (network_worker_submit(&job)) {
        s_app.next_id = job.request_id;
        s_app.next_pending = true;
    } else s_app.retry_at = now + 1000;
}

static void action(app_action_t value, int64_t now)
{
    if (value == APP_ACTION_NEXT) request_next(now);
    else if (value == APP_ACTION_RATING) ui_animation_show_rating_bar(s_app.state.rating);
    else if (value == APP_ACTION_RESTORE) ui_animation_update_bottom_bar(app_state_progress(&s_app.state, now));
    else if (value == APP_ACTION_SUBMIT) {
        network_job_t job = {.kind = NETWORK_RATE, .request_id = ++s_app.sequence,
            .item_id = s_app.state.item_id, .rating = s_app.state.rating};
        if (s_app.online && s_app.state.item_id && network_worker_submit(&job)) s_app.rating_id = job.request_id;
        else {
            record_error((net_result_t){.error = ESP_ERR_INVALID_STATE});
            app_state_rating_done(&s_app.state, now);
            ui_animation_show_submit_bar(false);
        }
    }
}

static void receive_results(int64_t now)
{
    network_result_t result;
    while (network_worker_receive(&result)) {
        if (result.job.kind == NETWORK_NEXT && result.job.request_id == s_app.next_id) {
            if (result.api.error == ESP_OK && s_app.online) {
                if (s_app.has_candidate) free(s_app.candidate.jpeg.data);
                s_app.candidate = result;
                s_app.has_candidate = true;
            } else {
                free(result.jpeg.data);
                s_app.next_pending = false;
                s_app.retry_at = now + 5000;
                record_error(result.api);
                if (!s_app.has_content) ui_animation_show_error();
            }
        } else if (result.job.kind == NETWORK_RATE && result.job.request_id == s_app.rating_id
            && result.job.item_id == s_app.state.item_id && s_app.state.view == APP_SUBMITTING) {
            record_error(result.api);
            app_state_rating_done(&s_app.state, now);
            ui_animation_show_submit_bar(result.api.error == ESP_OK);
        } else free(result.jpeg.data);
    }
    /* 用户评价期间保留原内容，候选内容等待评价完成后才切换 */
    if (s_app.has_candidate && (s_app.state.view == APP_NORMAL || s_app.state.view == APP_EMPTY)
        && !s_app.state.click_pending) {
        network_result_t *candidate = &s_app.candidate;
        esp_err_t err = content_view_show(&candidate->item, &candidate->jpeg);
        content_outcome_t outcome = !candidate->item.image_url[0] ? CONTENT_NO_IMAGE
            : candidate->image.error != ESP_OK ? CONTENT_DOWNLOAD_FALLBACK
            : err != ESP_OK ? CONTENT_DECODE_FALLBACK : CONTENT_IMAGE;
        net_result_t display_result = candidate->image;
        if (err != ESP_OK) display_result.error = err;
        record_error(display_result);
        int64_t shown_at = esp_timer_get_time() / 1000;
        s_app.shown_at = shown_at;
        s_app.has_content = true;
        app_state_shown(&s_app.state, candidate->item.id, shown_at);
        portENTER_CRITICAL(&s_lock);
        s_snapshot.item = candidate->item;
        s_snapshot.outcome = outcome;
        s_snapshot.last_success_ms = shown_at;
        if (display_result.error == ESP_OK) s_snapshot.failures = 0;
        portEXIT_CRITICAL(&s_lock);
        free(candidate->jpeg.data);
        candidate->jpeg = (http_blob_t){0};
        s_app.has_candidate = false;
        s_app.next_pending = false;
    }
}

static void connection_screen(const wifi_info_t *wifi)
{
    if (wifi->mode == MODE_AP) ui_animation_show_no_network(wifi->ap_ssid);
    else {
        app_config_t config;
        app_config_get_active(&config);
        char version[16];
        snprintf(version, sizeof(version), "v%d", CONFIG_FIRMWARE_VERSION);
        ui_connecting_info_t info = {.firmware = version, .ssid = wifi->ssid, .ip = wifi->ip,
            .backend_url = config.api_base_url[0] ? config.api_base_url : config.legacy_next_url};
        ui_animation_show_connecting(&info);
    }
}

esp_err_t app_controller_run(void)
{
    ui_animation_init();
    ui_animation_show_loading();
    esp_err_t err = app_config_init();
    if (err != ESP_OK) return err;
    if ((err = wifi_manager_init()) != ESP_OK || (err = button_input_init()) != ESP_OK
        || (err = network_worker_start()) != ESP_OK) return err;
    if (!http_server_start()) ESP_LOGW(TAG, "管理服务器未启动");
    if ((err = wifi_manager_start()) != ESP_OK) return err;
    s_app.last_wifi = (wifi_status_t)-1;
    s_app.last_mode = (wifi_work_mode_t)-1;
    while (true) {
        int64_t now = esp_timer_get_time() / 1000;
        portENTER_CRITICAL(&s_lock);
        bool restart = s_restart_at && now >= s_restart_at;
        portEXIT_CRITICAL(&s_lock);
        if (restart) esp_restart();
        wifi_info_t wifi;
        wifi_manager_get_snapshot(&wifi);
        bool online = wifi.status == WIFI_STATUS_CONNECTED;
        if (s_app.online && !online) {
            s_app.next_id = 0;
            s_app.next_pending = false;
            if (s_app.has_candidate) { free(s_app.candidate.jpeg.data); s_app.has_candidate = false; }
            s_app.retry_at = now;
        }
        s_app.online = online;
        if (!s_app.has_content && (wifi.status != s_app.last_wifi || wifi.mode != s_app.last_mode)) connection_screen(&wifi);
        s_app.last_wifi = wifi.status;
        s_app.last_mode = wifi.mode;
        int64_t timestamp;
        while (button_input_receive(&timestamp)) {
            /* 切图前的旧事件不作用于新内容 */
            if (s_app.has_content && timestamp >= s_app.shown_at) {
                action(app_state_tick(&s_app.state, timestamp), now);
                action(app_state_button(&s_app.state, timestamp), now);
            }
        }
        action(app_state_tick(&s_app.state, now), now);
        receive_results(now);
        if ((!s_app.has_content || s_app.state.next_requested) && s_app.state.view != APP_RATING
            && s_app.state.view != APP_SUBMITTING && s_app.state.view != APP_FEEDBACK) request_next(now);
        if (s_app.state.view == APP_NORMAL) ui_animation_update_bottom_bar(app_state_progress(&s_app.state, now));
        content_view_tick();
        unsigned stack = uxTaskGetStackHighWaterMark(NULL);
        portENTER_CRITICAL(&s_lock);
        s_snapshot.view = s_app.state.view;
        s_snapshot.network_busy = s_app.next_pending || s_app.state.view == APP_SUBMITTING;
        s_snapshot.ui_stack_free = stack;
        portEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
