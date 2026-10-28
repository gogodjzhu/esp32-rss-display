#include "wifi_manager.h"
#include "app_config.h"
#include "sdkconfig.h"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_EVENT);
enum { CMD_START, CMD_RECONNECT, CMD_DISCONNECT, CMD_TICK };
static const char *TAG = "WIFI_MGR";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_info_t s_info;
static bool s_initialized, s_wanted, s_attempting, s_rescue_ap, s_started;
static unsigned s_backoff;
static int64_t s_retry_at, s_attempt_deadline, s_rescue_at;
static esp_timer_handle_t s_timer;
static wifi_config_t s_sta;

void wifi_manager_get_snapshot(wifi_info_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_info;
    portEXIT_CRITICAL(&s_lock);
}

static void set_status(wifi_status_t status)
{
    portENTER_CRITICAL(&s_lock);
    s_info.status = status;
    if (status != WIFI_STATUS_CONNECTED) s_info.ip[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

static void retry_later(void)
{
    s_attempting = false;
    unsigned delay_ms = 1000U << (s_backoff < 5 ? s_backoff++ : 5);
    s_retry_at = esp_timer_get_time() + ((int64_t)delay_ms + esp_random() % 500) * 1000;
    portENTER_CRITICAL(&s_lock);
    s_info.retry_count++;
    s_info.status = WIFI_STATUS_DISCONNECTED;
    s_info.ip[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t enable_ap(bool concurrent)
{
    wifi_config_t ap = {0};
    size_t size = strlen(CONFIG_WIFI_AP_SSID);
    if (!size || size > 32 || strlen(CONFIG_WIFI_AP_PASSWORD) > 63) return ESP_ERR_INVALID_ARG;
    memcpy(ap.ap.ssid, CONFIG_WIFI_AP_SSID, size);
    ap.ap.ssid_len = size;
    strcpy((char *)ap.ap.password, CONFIG_WIFI_AP_PASSWORD);
    ap.ap.max_connection = 4;
    ap.ap.authmode = ap.ap.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_mode(concurrent ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&s_lock);
    s_info.mode = MODE_AP;
    strcpy(s_info.ap_ssid, CONFIG_WIFI_AP_SSID);
    if (!concurrent) {
        s_info.status = WIFI_STATUS_AP_MODE;
        strcpy(s_info.ip, "192.168.4.1");
    }
    portEXIT_CRITICAL(&s_lock);
    s_rescue_ap = true;
    return ESP_OK;
}

static void begin_attempt(void)
{
    if (!s_wanted || s_attempting) return;
    s_attempting = true;
    s_attempt_deadline = esp_timer_get_time() + (int64_t)CONFIG_WIFI_CONNECT_TIMEOUT_SEC * 1000000;
    set_status(WIFI_STATUS_CONNECTING);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) retry_later();
}

static void handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        begin_attempt();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        portENTER_CRITICAL(&s_lock);
        s_info.disconnect_reason = event->reason;
        portEXIT_CRITICAL(&s_lock);
        if (s_wanted) {
            if (!s_rescue_at) s_rescue_at = now + (int64_t)CONFIG_WIFI_CONNECT_TIMEOUT_SEC * 1000000;
            retry_later();
        }
        else { s_attempting = false; set_status(WIFI_STATUS_IDLE); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (!s_wanted) return;
        const ip_event_got_ip_t *event = data;
        char ip[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        portENTER_CRITICAL(&s_lock);
        s_info.mode = MODE_STA;
        s_info.status = WIFI_STATUS_CONNECTED;
        s_info.disconnect_reason = 0;
        strcpy(s_info.ip, ip);
        portEXIT_CRITICAL(&s_lock);
        s_attempting = false;
        s_backoff = 0;
        s_rescue_at = 0;
        ESP_LOGI(TAG, "已获得 IP: %s", ip);
    } else if (base == WIFI_MANAGER_EVENT) {
        if (id == CMD_START && !s_started) {
            s_started = true;
            esp_err_t err;
            if (s_wanted) {
                err = esp_wifi_set_mode(WIFI_MODE_STA);
                if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &s_sta);
                s_rescue_at = now + (int64_t)CONFIG_WIFI_CONNECT_TIMEOUT_SEC * 1000000;
            } else err = enable_ap(false);
            if (err == ESP_OK) err = esp_wifi_start();
            if (err != ESP_OK) { set_status(WIFI_STATUS_IDLE); ESP_LOGE(TAG, "启动失败: %s", esp_err_to_name(err)); }
        } else if (id == CMD_DISCONNECT) {
            s_wanted = false;
            s_attempting = false;
            set_status(WIFI_STATUS_IDLE);
            esp_wifi_disconnect();
        } else if (id == CMD_RECONNECT && s_sta.sta.ssid[0]) {
            s_wanted = true;
            s_backoff = 0;
            s_attempting = false;
            s_retry_at = now + 500000;
            s_rescue_at = now + (int64_t)CONFIG_WIFI_CONNECT_TIMEOUT_SEC * 1000000;
            set_status(WIFI_STATUS_DISCONNECTED);
            esp_wifi_disconnect();
        } else if (id == CMD_TICK && s_started && s_wanted) {
            wifi_info_t snapshot;
            wifi_manager_get_snapshot(&snapshot);
            if (snapshot.status == WIFI_STATUS_CONNECTED) return;
            if (!s_rescue_ap && s_rescue_at && now >= s_rescue_at) {
                esp_err_t err = enable_ap(true);
                if (err != ESP_OK) ESP_LOGW(TAG, "配网热点启动失败: %s", esp_err_to_name(err));
            }
            if (s_attempting && now >= s_attempt_deadline) {
                s_attempting = false;
                esp_wifi_disconnect();
                retry_later();
            } else if (!s_attempting && now >= s_retry_at) begin_attempt();
        }
    }
}

static void tick(void *arg)
{
    (void)arg;
    /* 定时器任务只投递，所有连接操作在同一事件循环执行 */
    esp_event_post(WIFI_MANAGER_EVENT, CMD_TICK, NULL, 0, 0);
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;
    if (!esp_netif_create_default_wifi_sta() || !esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&wifi)) != ESP_OK) return err;
    /* 凭证只由配置服务保存，驱动不维护第二份持久化副本 */
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handler, NULL)) != ESP_OK
        || (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handler, NULL)) != ESP_OK
        || (err = esp_event_handler_register(WIFI_MANAGER_EVENT, ESP_EVENT_ANY_ID, handler, NULL)) != ESP_OK) return err;
    app_config_t config;
    app_config_get_active(&config);
    memcpy(s_sta.sta.ssid, config.wifi_ssid, strlen(config.wifi_ssid));
    memcpy(s_sta.sta.password, config.wifi_password, strlen(config.wifi_password));
    s_sta.sta.threshold.authmode = config.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_sta.sta.pmf_cfg.capable = true;
    strcpy(s_info.ssid, config.wifi_ssid);
    s_wanted = config.wifi_ssid[0] != '\0';
    esp_timer_create_args_t timer = {.callback = tick, .name = "wifi_retry"};
    if ((err = esp_timer_create(&timer, &s_timer)) != ESP_OK) return err;
    if ((err = esp_timer_start_periodic(s_timer, 500000)) != ESP_OK) return err;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    return s_initialized ? esp_event_post(WIFI_MANAGER_EVENT, CMD_START, NULL, 0, 0) : ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_manager_reconnect(void)
{
    if (!s_initialized || !s_sta.sta.ssid[0]) return ESP_ERR_INVALID_STATE;
    return esp_event_post(WIFI_MANAGER_EVENT, CMD_RECONNECT, NULL, 0, 0);
}

esp_err_t wifi_manager_disconnect(void)
{
    return s_initialized ? esp_event_post(WIFI_MANAGER_EVENT, CMD_DISCONNECT, NULL, 0, 0) : ESP_ERR_INVALID_STATE;
}
