# deps_src/simdjson/CMakeLists.txt
cmake_minimum_required(VERSION 3.13)
project(simdjson)

add_library(simdjson STATIC
    simdjson.cpp
)

target_include_directories(simdjson SYSTEM
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(simdjson PUBLIC cxx_std_17)
set_target_properties(simdjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(simdjson::simdjson ALIAS simdjson)
