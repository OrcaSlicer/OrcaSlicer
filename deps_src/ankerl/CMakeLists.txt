cmake_minimum_required(VERSION 3.13)
project(ankerl)

add_library(ankerl INTERFACE)

target_include_directories(ankerl SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(ankerl INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/unordered_dense.h
)
