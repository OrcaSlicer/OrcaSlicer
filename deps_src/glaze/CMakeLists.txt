cmake_minimum_required(VERSION 3.13)

project(glaze)

add_library(glaze INTERFACE)
add_library(glaze::glaze ALIAS glaze)

target_include_directories(glaze SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_sources(glaze INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/glaze/glaze.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/glaze/glaze_exceptions.hpp
)

# Require C++23 for Glaze
target_compile_features(glaze INTERFACE cxx_std_23)
