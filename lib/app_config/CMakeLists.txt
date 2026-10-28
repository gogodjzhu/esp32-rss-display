# 显式声明源码和模块依赖
idf_component_register(SRCS "app_config.c" "config_model.c"
    INCLUDE_DIRS "." REQUIRES nvs_manager json freertos)
