# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

# Collect source files from src/ and lib/
FILE(GLOB_RECURSE app_sources 
    ${CMAKE_SOURCE_DIR}/src/*.*
    ${CMAKE_SOURCE_DIR}/lib/*/*.*
)

idf_component_register(SRCS ${app_sources})
