cmake_minimum_required(VERSION 3.13)

project(miniz)

add_library(miniz INTERFACE)

add_library(miniz_static STATIC
    miniz.c
    miniz.h
)

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
    target_compile_definitions(miniz_static PRIVATE _GNU_SOURCE)
endif()

target_link_libraries(miniz INTERFACE miniz_static)
target_include_directories(miniz SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

