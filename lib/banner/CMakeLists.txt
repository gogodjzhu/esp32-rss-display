# 显式声明源码和模块依赖
idf_component_register(SRCS "banner.c"
    INCLUDE_DIRS "." REQUIRES cjk_font display_driver lvgl esp_timer log text_utf8)
