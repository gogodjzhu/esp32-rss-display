# 显式声明源码和模块依赖
idf_component_register(SRCS "network_worker.c"
    INCLUDE_DIRS "." REQUIRES backend_client app_config freertos)
