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

/**
 * @brief WiFi工作模式
 */
typedef enum {
    MODE_STA,   /**< Station模式 */
    MODE_AP     /**< AP模式 */
} wifi_work_mode_t;

/**
 * @brief 初始化并连接WiFi
 * @param mode 工作模式: MODE_STA 或 MODE_AP
 * @note STA模式阻塞直到连接成功或失败
 * @return ESP_OK 成功，其他失败
 */
esp_err_t wifi_manager_connect(wifi_work_mode_t mode);

/**
 * @brief 获取WiFi的IP地址
 * @return 指向ip4_addr_t的指针，如果未连接返回NULL
 */
esp_ip4_addr_t* wifi_manager_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
