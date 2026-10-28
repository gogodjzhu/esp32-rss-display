# 显式声明源码和模块依赖
idf_component_register(SRCS "content_view.c"
    INCLUDE_DIRS "." REQUIRES content http_transport ui_animation jpeg_decoder banner display_driver)
