add_library(orcaslicer_render_ios_metal STATIC
    IOSMetalRenderBackend.cpp
    IOSMetalRenderBackend.hpp
)

target_link_libraries(orcaslicer_render_ios_metal
    PUBLIC
        orcaslicer_portability_api
)

target_include_directories(orcaslicer_render_ios_metal
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
