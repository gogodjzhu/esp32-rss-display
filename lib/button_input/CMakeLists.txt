# 显式声明源码和模块依赖
idf_component_register(SRCS "button_input.c"
    INCLUDE_DIRS "." REQUIRES display_driver esp_driver_gpio esp_timer freertos)
