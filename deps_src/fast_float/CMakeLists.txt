cmake_minimum_required(VERSION 3.13)
project(fast_float)

add_library(fast_float INTERFACE)

target_include_directories(fast_float SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(fast_float INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/fast_float.h
)

# Require C++14 for fast_float
target_compile_features(fast_float INTERFACE cxx_std_14)
