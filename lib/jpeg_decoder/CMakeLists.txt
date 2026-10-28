# 显式声明源码和模块依赖
idf_component_register(SRCS "jpeg_decoder.c"
    INCLUDE_DIRS "." REQUIRES esp_rom freertos)
