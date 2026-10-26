# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

# LVGL 位于 .pio/libdeps/<env名>/lvgl（PlatformIO 按环境分别缓存依赖，esp32s3/esp32c3
# 双环境并存时会有两个目录）。此处无法从 CMAKE_BINARY_DIR 可靠推断当前 env（组件
# 需求扫描阶段它尚未指向 build 目录），故用"版本 + 最近访问时间"双校验选出当前
# 构建实际使用的 LVGL 缓存目录：
#   1. 版本必须与 platformio.ini 中 lib_deps 声明的 ~9.6.0 兼容
#      （读取 env_support/cmake/version.cmake 中的 LVGL_VERSION_{MAJOR,MINOR}）
#   2. 多目录命中时优先最近被 PlatformIO 访问过的（当前 env 刚拉取/使用过依赖）
FILE(GLOB LVGL_CANDIDATES "${CMAKE_SOURCE_DIR}/.pio/libdeps/*/lvgl")
SET(LVGL_DIR "")
SET(__lvgl_match "")
FOREACH(_cand ${LVGL_CANDIDATES})
    FILE(READ "${_cand}/env_support/cmake/version.cmake" _ver_file)
    IF(NOT _ver_file)
        CONTINUE()
    ENDIF()
    # 用独立变量保存两次 REGEX 匹配结果，避免 CMAKE_MATCH_<n> 互相覆盖
    STRING(REGEX MATCH "LVGL_VERSION_MAJOR[ \\t]*\"([0-9]+)\"" _unused1 "${_ver_file}")
    SET(_lvgl_maj "${CMAKE_MATCH_1}")
    STRING(REGEX MATCH "LVGL_VERSION_MINOR[ \\t]*\"([0-9]+)\"" _unused2 "${_ver_file}")
    SET(_lvgl_min "${CMAKE_MATCH_1}")
    # 仅接受 9.6.x（与 platformio.ini lib_deps 的 ~9.6.0 锁定一致）
    IF("${_lvgl_maj}" STREQUAL "9" AND "${_lvgl_min}" STREQUAL "6")
        LIST(APPEND __lvgl_match "${_cand}")
    ENDIF()
ENDFOREACH()
IF(NOT DEFINED __lvgl_match OR __lvgl_match STREQUAL "")
    MESSAGE(FATAL_ERROR "未找到兼容的 LVGL 9.6 缓存目录（.pio/libdeps/*/lvgl），请先执行 pio run 拉取依赖")
ENDIF()
LIST(LENGTH __lvgl_match __lvgl_match_count)
IF(__lvgl_match_count GREATER 1)
    # 多环境并存：取最近被访问过的（FileTime 在 Windows/Unix 均可用）
    SET(LVGL_DIR "")
    SET(__latest "0")
    FOREACH(_cand ${__lvgl_match})
        FILE(TIMESTAMP "${_cand}/env_support/cmake/version.cmake" _mtime "%s" UTC)
        IF("${_mtime}" GREATER "${__latest}")
            SET(__latest "${_mtime}")
            SET(LVGL_DIR "${_cand}")
        ENDIF()
    ENDFOREACH()
    MESSAGE(STATUS "检测到多个 LVGL 缓存（${__lvgl_match}），选择最近使用: ${LVGL_DIR}")
ELSE()
    LIST(GET __lvgl_match 0 LVGL_DIR)
ENDIF()
MESSAGE(STATUS "使用 LVGL 缓存目录: ${LVGL_DIR}")

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
