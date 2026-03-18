add_library(orcaslicer_render_ios_metal STATIC
    IOSMetalRenderBackend.mm
    IOSMetalRenderBackend.hpp
)

target_link_libraries(orcaslicer_render_ios_metal
    PUBLIC
        orcaslicer_portability_render_interface
    PRIVATE
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
)

target_include_directories(orcaslicer_render_ios_metal
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
