# 显式声明源码和模块依赖
idf_component_register(SRCS "cjk_font.c"
    INCLUDE_DIRS "." REQUIRES lvgl esp_partition esp_system log)
