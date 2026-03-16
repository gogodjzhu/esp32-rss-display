/**
 * @file wifi_manager.h
 * @brief WiFi Manager - 负责WiFi连接管理
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_NVS_NAMESPACE  "wifi_config"
#define WIFI_NVS_KEY_SSID   "ssid"
#define WIFI_NVS_KEY_PASS   "password"
#define WIFI_NVS_KEY_VER    "fw_ver"        // 固件版本号，用于检测升级
#define WIFI_FIRMWARE_VER   4               // 固件版本号，升级时需要清空NVS
#define WIFI_AP_SSID        "ESP32-Config"
#define WIFI_AP_PASSWORD    "12345678"

/**
 * @brief WiFi工作模式
 */
typedef enum {
    MODE_STA,   /**< Station模式 */
    MODE_AP     /**< AP模式 */
} wifi_work_mode_t;

/**
 * @brief WiFi连接状态
 */
typedef enum {
    WIFI_STATUS_IDLE,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_AP_MODE
} wifi_status_t;

/**
 * @brief WiFi连接信息结构体
 */
typedef struct {
    wifi_work_mode_t mode;
    wifi_status_t status;
    char ssid[32];
    char ip[16];
    char ap_ssid[32];
} wifi_info_t;

/**
 * @brief 初始化WiFi Manager (调用一次)
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 根据配置智能连接WiFi
 * @return ESP_OK 成功, ESP_FAIL 需要进入AP配置模式
 */
esp_err_t wifi_manager_smart_connect(void);

/**
 * @brief 切换到AP模式 (配网模式)
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_start_ap_mode(void);

/**
 * @brief 切换到STA模式并使用保存的凭证连接
 * @return ESP_OK 成功, ESP_FAIL 连接失败
 */
esp_err_t wifi_manager_start_sta_mode(void);

/**
 * @brief 保存WiFi凭证到NVS
 * @param ssid WiFi名称
 * @param password WiFi密码
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

/**
 * @brief 加载WiFi凭证从NVS
 * @param ssid 存储ssid的缓冲区
 * @param password 存储password的缓冲区
 * @param max_len 缓冲区最大长度
 * @return ESP_OK 成功, ESP_ERR_NOT_FOUND 未找到凭证
 */
esp_err_t wifi_manager_load_credentials(char *ssid, char *password, size_t max_len);

/**
 * @brief 检查是否已有保存的WiFi凭证
 * @return true 有保存的凭证, false 没有保存的凭证
 */
bool wifi_manager_has_credentials(void);

/**
 * @brief 清除保存的WiFi凭证
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_clear_credentials(void);

/**
 * @brief 获取WiFi信息
 * @return 指向wifi_info_t的指针
 */
wifi_info_t* wifi_manager_get_info(void);

/**
 * @brief 获取WiFi的IP地址
 * @return 指向ip4_addr_t的指针，如果未连接返回NULL
 */
esp_ip4_addr_t* wifi_manager_get_ip(void);

/**
 * @brief 重新连接WiFi
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_reconnect(void);

/**
 * @brief 断开WiFi连接
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
