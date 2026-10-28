# 显式声明源码和模块依赖
idf_component_register(SRCS "nvs_manager.c"
    INCLUDE_DIRS "." REQUIRES nvs_flash log)
