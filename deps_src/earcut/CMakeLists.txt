cmake_minimum_required(VERSION 3.13)
project(earcut)

add_library(earcut INTERFACE)

target_include_directories(earcut SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(earcut INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/earcut.hpp
)
