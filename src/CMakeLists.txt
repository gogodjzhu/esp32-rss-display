# 应用控制与管理 API 共用同一应用服务边界
idf_component_register(
    SRCS "main.cpp"
         "../lib/app_controller/app_controller.c"
         "../lib/app_controller/app_state.c"
         "../lib/http_server/http_server.c"
    INCLUDE_DIRS "." "../lib/app_controller" "../lib/http_server"
    PRIV_REQUIRES app_config button_input content_view network_worker ui_animation wifi
                  esp_timer esp_system esp_http_server spiffs json heap ui_limits)
