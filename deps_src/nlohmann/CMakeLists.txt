cmake_minimum_required(VERSION 3.13)

project(nlohmann_json)

add_library(nlohmann_json INTERFACE)

target_include_directories(nlohmann_json SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/json.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/json_fwd.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/adl_serializer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/byte_container_with_subtype.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ordered_map.hpp
)

# Require C++11 for nlohmann/json
target_compile_features(nlohmann_json INTERFACE cxx_std_11)
