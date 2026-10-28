# 显式声明源码和模块依赖
idf_component_register(SRCS "display_driver.c"
    INCLUDE_DIRS "." REQUIRES esp_driver_spi esp_driver_gpio freertos)
