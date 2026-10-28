# 显式声明源码和模块依赖
idf_component_register(SRCS "wifi_manager.c"
    INCLUDE_DIRS "." REQUIRES app_config esp_wifi esp_event esp_netif esp_timer esp_system log)
