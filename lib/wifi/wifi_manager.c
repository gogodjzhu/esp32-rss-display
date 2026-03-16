/**
 * @file wifi_manager.c
 * @brief WiFi Manager 实现
 */

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include "wifi_manager.h"

static const char *TAG = "WIFI_MGR";

// WiFi连接超时时间 (10分钟)
#define WIFI_CONNECT_TIMEOUT_MS   (600000)
// WiFi重连检测间隔 (500ms)
#define WIFI_RETRY_INTERVAL_MS    (500)

// 静态变量定义
static esp_ip4_addr_t s_ip_addr;           // IP地址
static bool s_connected = false;           // 连接状态标志
static bool s_initialized = false;         // 初始化标志
static wifi_info_t s_wifi_info = {0};      // WiFi信息结构体
static bool s_connecting = false;          // 正在连接标志
static bool s_ap_mode = false;             // AP模式标志

// WiFi事件处理函数
// 处理STA连接、断开、获取IP等事件
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    // STA模式启动事件
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    }
    // STA断开连接事件
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", disconnected->reason);
        s_connected = false;
        s_wifi_info.status = WIFI_STATUS_DISCONNECTED;
        
        // 如果正在连接模式，断开后自动重连
        if (s_connecting) {
            ESP_LOGI(TAG, "Reconnecting in progress... (ssid: %s)", s_wifi_info.ssid);
            s_wifi_info.status = WIFI_STATUS_CONNECTING;
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "Not in connecting mode, will not auto-reconnect");
        }
    }
    // 获取到IP地址事件
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_ip_addr = event->ip_info.ip;
        s_connected = true;
        s_connecting = false;
        s_wifi_info.status = WIFI_STATUS_CONNECTED;
        esp_ip4addr_ntoa(&s_ip_addr, s_wifi_info.ip, sizeof(s_wifi_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_wifi_info.ip);
    }
    // AP模式下有客户端连接事件
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Station connected to AP");
    }
    // AP模式下有客户端断开事件
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Station disconnected from AP");
    }
}

// 初始化WiFi Manager
// 初始化NVS、TCP/IP协议栈、WiFi驱动等
esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi Manager already initialized, skipping");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi Manager...");

    ESP_LOGI(TAG, "Initializing NVS...");
    ESP_ERROR_CHECK(nvs_manager_init());

    ESP_LOGI(TAG, "Initializing TCP/IP stack...");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "Creating default event loop...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Initializing WiFi driver...");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_initialized = true;
    s_wifi_info.status = WIFI_STATUS_IDLE;
    ESP_LOGI(TAG, "WiFi Manager initialized successfully");
    return ESP_OK;
}

// 保存WiFi凭证到NVS存储
// 参数: ssid - WiFi名称, password - WiFi密码
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Saving credentials: ssid=%s, password=%s", ssid, password);
    esp_err_t err = nvs_manager_set_str(WIFI_NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_manager_set_str(WIFI_NVS_KEY_PASS, password);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "WiFi credentials saved");
    return ESP_OK;
}

// 从NVS加载WiFi凭证
// 参数: ssid/password - 存储缓冲区, max_len - 缓冲区长度
// 返回: ESP_OK成功, ESP_ERR_NOT_FOUND未找到
esp_err_t wifi_manager_load_credentials(char *ssid, char *password, size_t max_len)
{
    ESP_LOGI(TAG, "Loading WiFi credentials from NVS...");
    size_t ssid_len = max_len;
    size_t pass_len = max_len;
    
    esp_err_t err = nvs_manager_get_str(WIFI_NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No SSID found in NVS (err=%d)", err);
        return err;
    }

    err = nvs_manager_get_str(WIFI_NVS_KEY_PASS, password, &pass_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded credentials: ssid=%s", ssid);
    } else {
        ESP_LOGI(TAG, "SSID found but no password in NVS (err=%d)", err);
    }

    return err;
}

// 检查是否已保存WiFi凭证
// 返回: true - 已保存, false - 未保存
bool wifi_manager_has_credentials(void)
{
    bool has = nvs_manager_has_key(WIFI_NVS_KEY_SSID);
    ESP_LOGI(TAG, "Checking saved credentials: %s", has ? "FOUND" : "NOT FOUND");
    return has;
}

// 清除保存的WiFi凭证
// 用于重置到出厂配置模式
esp_err_t wifi_manager_clear_credentials(void)
{
    esp_err_t err = nvs_manager_erase(WIFI_NVS_KEY_SSID);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    err = nvs_manager_erase(WIFI_NVS_KEY_PASS);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    ESP_LOGI(TAG, "WiFi credentials cleared");
    return ESP_OK;
}

// 启动AP模式 (配置模式)
// 创建一个WiFi热点, 供用户连接并访问配置页面
esp_err_t wifi_manager_start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP mode...");
    s_ap_mode = true;
    s_connected = false;
    s_connecting = false;
    s_wifi_info.mode = MODE_AP;
    s_wifi_info.status = WIFI_STATUS_AP_MODE;
    strcpy(s_wifi_info.ap_ssid, WIFI_AP_SSID);

    ESP_LOGI(TAG, "Creating AP network interface...");
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.ap.ssid, WIFI_AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    strncpy((char*)wifi_config.ap.password, WIFI_AP_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: ssid=%s, password=%s, max_conn=%d", WIFI_AP_SSID, WIFI_AP_PASSWORD, 4);

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ap_netif, &ip_info);
        esp_ip4addr_ntoa(&ip_info.ip, s_wifi_info.ip, sizeof(s_wifi_info.ip));
        ESP_LOGI(TAG, "AP IP: %s, config page: http://%s", s_wifi_info.ip, s_wifi_info.ip);
    }
    ESP_LOGI(TAG, "Waiting for user to connect and configure WiFi credentials...");

    return ESP_OK;
}

// 启动STA模式 (正常连接模式)
// 使用保存的凭证连接WiFi, 阻塞等待连接或超时
esp_err_t wifi_manager_start_sta_mode(void)
{
    ESP_LOGI(TAG, "Starting STA mode...");
    char ssid[32] = {0};
    char password[64] = {0};

    if (wifi_manager_load_credentials(ssid, password, sizeof(ssid)) != ESP_OK) {
        ESP_LOGE(TAG, "No WiFi credentials found, cannot start STA mode");
        return ESP_FAIL;
    }

    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Empty WiFi SSID, cannot start STA mode");
        return ESP_FAIL;
    }

    s_ap_mode = false;
    s_connecting = true;
    s_connected = false;
    s_wifi_info.mode = MODE_STA;
    s_wifi_info.status = WIFI_STATUS_CONNECTING;
    strncpy(s_wifi_info.ssid, ssid, sizeof(s_wifi_info.ssid) - 1);

    ESP_LOGI(TAG, "Creating STA network interface...");
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to ssid=%s, password=%s (timeout: %ds)...", ssid, password, WIFI_CONNECT_TIMEOUT_MS / 1000);

    // 等待连接成功或超时 (最多10分钟)
    int64_t start_time = esp_timer_get_time() / 1000;
    int64_t elapsed = 0;
    
    while (!s_connected && elapsed < WIFI_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_INTERVAL_MS));
        elapsed = esp_timer_get_time() / 1000 - start_time;
        
        // 每30秒打印一次日志
        if (elapsed > 0 && elapsed % 30000 < WIFI_RETRY_INTERVAL_MS) {
            ESP_LOGI(TAG, "Still trying to connect... (%lld/%d seconds)", elapsed / 1000, WIFI_CONNECT_TIMEOUT_MS / 1000);
        }
    }

    // 连接成功
    if (s_connected) {
        s_connecting = false;
        s_wifi_info.status = WIFI_STATUS_CONNECTED;
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else {
        // 连接超时, 保持重连状态
        ESP_LOGW(TAG, "Connection timeout after %d seconds, will retry in background", WIFI_CONNECT_TIMEOUT_MS / 1000);
        s_connecting = true;
        s_wifi_info.status = WIFI_STATUS_CONNECTING;
        return ESP_ERR_TIMEOUT;
    }
}

// 智能WiFi连接
// 检查是否有保存的凭证, 有则尝试STA模式, 否则进入AP模式
esp_err_t wifi_manager_smart_connect(void)
{
    ESP_LOGI(TAG, "Smart connect: deciding WiFi mode...");
    if (!s_initialized) {
        wifi_manager_init();
    }

    if (wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Credentials found, entering STA mode");
        
        esp_err_t ret = wifi_manager_start_sta_mode();
        
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        
        ESP_LOGW(TAG, "Initial connection timed out, will keep retrying in background for up to %d seconds", WIFI_CONNECT_TIMEOUT_MS / 1000);
        
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "No credentials found, entering AP config mode");
    }

    wifi_manager_start_ap_mode();
    return ESP_FAIL;
}

// 获取WiFi IP地址
// 返回: 指向ip4_addr_t的指针, 未连接返回NULL
esp_ip4_addr_t* wifi_manager_get_ip(void)
{
    return s_connected ? &s_ip_addr : NULL;
}

// 获取WiFi连接信息
// 返回: 指向wifi_info_t的指针
wifi_info_t* wifi_manager_get_info(void)
{
    return &s_wifi_info;
}

// 手动触发WiFi重连
// 用于用户在页面上点击重连按钮
esp_err_t wifi_manager_reconnect(void)
{
    ESP_LOGI(TAG, "Manual reconnect triggered");
    s_connecting = true;
    s_wifi_info.status = WIFI_STATUS_CONNECTING;
    return esp_wifi_connect();
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting WiFi...");
    s_connecting = false;
    s_connected = false;
    s_wifi_info.status = WIFI_STATUS_IDLE;
    return esp_wifi_disconnect();
}
