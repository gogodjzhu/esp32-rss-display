# 显式声明源码和模块依赖
idf_component_register(SRCS "ui_animation.c"
    INCLUDE_DIRS "." REQUIRES display_driver banner cjk_font lvgl esp_timer log)
