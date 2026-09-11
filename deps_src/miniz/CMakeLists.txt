cmake_minimum_required(VERSION 3.13)

project(miniz)

add_library(miniz INTERFACE)

add_library(miniz_static STATIC
    miniz.c
    miniz.h
)

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
    target_compile_definitions(miniz_static PRIVATE _GNU_SOURCE)
elseif (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(miniz_static PRIVATE /clang:-Wno-error=incompatible-pointer-types)
endif()

target_link_libraries(miniz INTERFACE miniz_static)
target_include_directories(miniz SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

