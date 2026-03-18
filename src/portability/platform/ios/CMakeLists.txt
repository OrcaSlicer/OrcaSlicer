add_library(orcaslicer_platform_ios STATIC
    IOSKeychainCredentialStore.hpp
    IOSKeychainCredentialStore.mm
    IOSPlatformServices.hpp
    IOSPlatformServices.mm
)

target_link_libraries(orcaslicer_platform_ios
    PUBLIC
        orcaslicer_portability_platform_interface
        "-framework Foundation"
        "-framework Security"
)

target_include_directories(orcaslicer_platform_ios
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..
)
