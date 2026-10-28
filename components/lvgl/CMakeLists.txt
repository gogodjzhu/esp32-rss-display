# 唯一环境的依赖路径；原生 IDF 构建可以显式覆盖
set(LVGL_DIR "${CMAKE_SOURCE_DIR}/.pio/libdeps/esp32s3/lvgl" CACHE PATH "LVGL 源码目录")
if(NOT EXISTS "${LVGL_DIR}/env_support/cmake/version.cmake")
    message(FATAL_ERROR "未找到 LVGL，请执行 pio pkg install 或指定 LVGL_DIR")
endif()
include("${LVGL_DIR}/env_support/cmake/version.cmake")
if(NOT LVGL_VERSION_MAJOR EQUAL 9 OR NOT LVGL_VERSION_MINOR EQUAL 6 OR NOT LVGL_VERSION_PATCH EQUAL 0)
    message(FATAL_ERROR "本项目需要 LVGL 9.6.0")
endif()
file(GLOB_RECURSE lvgl_sources "${LVGL_DIR}/src/*.c")
idf_component_register(SRCS ${lvgl_sources}
    INCLUDE_DIRS "${LVGL_DIR}" "${LVGL_DIR}/src" "${CMAKE_SOURCE_DIR}/src")
