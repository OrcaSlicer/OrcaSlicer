cmake_minimum_required(VERSION 3.13)

project(libigl)

add_library(libigl INTERFACE)

find_package(libigl QUIET)

if(libigl_FOUND)
    message(STATUS "IGL found, using system version...")
    target_link_libraries(libigl INTERFACE igl::core)
else()
    message(STATUS "IGL NOT found, using bundled version...")
    target_include_directories(libigl SYSTEM BEFORE
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR})
endif()
