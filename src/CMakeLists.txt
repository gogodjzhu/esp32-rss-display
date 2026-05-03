# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

set(LVGL_DIR "${CMAKE_SOURCE_DIR}/.pio/libdeps/airm2m_core_esp32c3/lvgl")

# Collect source files from src/ and lib/
FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.*
    ${CMAKE_SOURCE_DIR}/lib/*/*.*
)

# LVGL core sources
FILE(GLOB_RECURSE lvgl_sources ${LVGL_DIR}/src/*.c)

# LVGL demo sources (benchmark + widgets, each as separate object to avoid symbol conflicts)
set(lvgl_demo_sources
    ${LVGL_DIR}/demos/lv_demos.c
    ${LVGL_DIR}/demos/benchmark/lv_demo_benchmark.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_12_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_14_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_16_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_18_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_20_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_24_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/lv_font_benchmark_montserrat_26_aligned.c
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_avatar.c
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_lvgl_logo_argb.c
    ${LVGL_DIR}/demos/benchmark/assets/img_benchmark_lvgl_logo_rgb.c
    ${LVGL_DIR}/demos/widgets/lv_demo_widgets.c
    ${LVGL_DIR}/demos/widgets/lv_demo_widgets_analytics.c
    ${LVGL_DIR}/demos/widgets/lv_demo_widgets_components.c
    ${LVGL_DIR}/demos/widgets/lv_demo_widgets_profile.c
    ${LVGL_DIR}/demos/widgets/lv_demo_widgets_shop.c
    ${LVGL_DIR}/demos/widgets/assets/img_clothes.c
    ${LVGL_DIR}/demos/widgets/assets/img_demo_widgets_avatar.c
    ${LVGL_DIR}/demos/widgets/assets/img_demo_widgets_needle.c
    ${LVGL_DIR}/demos/widgets/assets/img_lvgl_logo.c
)

# Collect lib include directories (only real directories)
FILE(GLOB lib_subdirs LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/lib/*)
set(lib_include_dirs "")
foreach(dir ${lib_subdirs})
    if(IS_DIRECTORY ${dir})
        list(APPEND lib_include_dirs ${dir})
    endif()
endforeach()

idf_component_register(SRCS ${app_sources} ${lvgl_sources} ${lvgl_demo_sources}
                       INCLUDE_DIRS
                           ${CMAKE_SOURCE_DIR}/src
                           ${lib_include_dirs}
                           ${LVGL_DIR}
                           ${LVGL_DIR}/src
                           ${LVGL_DIR}/demos
                       REQUIRES spiffs esp_timer log)
