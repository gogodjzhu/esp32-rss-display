# 显式声明源码和模块依赖
idf_component_register(SRCS "board_led.c"
    INCLUDE_DIRS "." REQUIRES board_config esp_driver_gpio)
