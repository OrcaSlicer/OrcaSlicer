add_library(orcaslicer_portability_ios STATIC
    IOSPlatformServices.cpp
    IOSPlatformServices.hpp
)

target_link_libraries(orcaslicer_portability_ios
    PUBLIC
        orcaslicer_portability_api
)
