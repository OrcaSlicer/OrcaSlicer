cmake_minimum_required(VERSION 3.13)

project(imguizmo)

add_library(imguizmo STATIC
    ImGuizmo.h
    ImGuizmo.cpp
)

target_include_directories(imguizmo SYSTEM
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(imguizmo PUBLIC imgui)
