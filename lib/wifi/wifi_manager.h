#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
#include <stdbool.h>
#include <esp_err.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { MODE_STA, MODE_AP } wifi_work_mode_t;
typedef enum {
    WIFI_STATUS_IDLE, WIFI_STATUS_CONNECTING, WIFI_STATUS_CONNECTED,
    WIFI_STATUS_DISCONNECTED, WIFI_STATUS_AP_MODE
} wifi_status_t;
typedef struct {
    wifi_work_mode_t mode;
    wifi_status_t status;
    char ssid[33];
    char ip[16];
    char ap_ssid[33];
    int disconnect_reason;
    unsigned retry_count;
} wifi_info_t;
esp_err_t wifi_manager_init(void);
/* 只发起连接，获得 IP 通过状态快照判断 */
esp_err_t wifi_manager_start(void);
void wifi_manager_get_snapshot(wifi_info_t *out);
/* 只查询是否具备重连条件，不触发任何 WiFi 操作 */
bool wifi_manager_can_reconnect(void);
esp_err_t wifi_manager_reconnect(void);
esp_err_t wifi_manager_disconnect(void);
#ifdef __cplusplus
}
#endif
#endif
