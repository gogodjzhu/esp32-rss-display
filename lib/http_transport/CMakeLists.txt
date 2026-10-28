# 显式声明源码和模块依赖
idf_component_register(SRCS "http_transport.c"
    INCLUDE_DIRS "." REQUIRES app_config esp_http_client mbedtls json)
