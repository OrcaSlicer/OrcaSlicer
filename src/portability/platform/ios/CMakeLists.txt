add_library(orcaslicer_platform_ios STATIC
    IOSPlatformServices.cpp
    IOSPlatformServices.hpp
)

target_link_libraries(orcaslicer_platform_ios
    PUBLIC
        orcaslicer_portability_api
)

target_include_directories(orcaslicer_platform_ios
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
