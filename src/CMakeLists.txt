# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

set(LVGL_DIR "${CMAKE_SOURCE_DIR}/.pio/libdeps/airm2m_core_esp32c3/lvgl")

# Collect source files from src/ and lib/
FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.*
    ${CMAKE_SOURCE_DIR}/lib/*/*.*
)

# LVGL core sources (仅 .c 文件，排除 ARM Helium 汇编)
FILE(GLOB_RECURSE lvgl_sources ${LVGL_DIR}/src/*.c)

# Collect lib include directories (only real directories)
FILE(GLOB lib_subdirs LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/lib/*)
set(lib_include_dirs "")
foreach(dir ${lib_subdirs})
    if(IS_DIRECTORY ${dir})
        list(APPEND lib_include_dirs ${dir})
    endif()
endforeach()

idf_component_register(SRCS ${app_sources} ${lvgl_sources}
                       INCLUDE_DIRS
                           ${CMAKE_SOURCE_DIR}/src
                           ${lib_include_dirs}
                           ${LVGL_DIR}
                           ${LVGL_DIR}/src
                       REQUIRES spiffs esp_timer log)
