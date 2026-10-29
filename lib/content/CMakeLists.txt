# 显式声明源码和模块依赖
idf_component_register(SRCS "content_item.c"
    INCLUDE_DIRS "." REQUIRES app_config json text_utf8)
