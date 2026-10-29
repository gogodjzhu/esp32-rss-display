# 显式声明源码和模块依赖
idf_component_register(SRCS "backend_client.c"
    INCLUDE_DIRS "." REQUIRES content http_transport json ui_limits)
