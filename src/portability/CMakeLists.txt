option(ORCASLICER_BUILD_IOS_PORTABILITY "Build iOS portability services" ON)
option(ORCASLICER_BUILD_IOS_SMOKE_TARGET "Build iOS smoke target for portability link validation" ON)

set(ORCASLICER_IOS_TOOLCHAIN_ACTIVE OFF)
if (APPLE AND ORCASLICER_BUILD_IOS_PORTABILITY)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(ORCASLICER_IOS_TOOLCHAIN_ACTIVE ON)
    elseif (CMAKE_OSX_SYSROOT MATCHES "iphoneos|iphonesimulator")
        set(ORCASLICER_IOS_TOOLCHAIN_ACTIVE ON)
    endif ()
endif ()

message(STATUS "iOS portability toolchain active: ${ORCASLICER_IOS_TOOLCHAIN_ACTIVE}")

if (ORCASLICER_IOS_TOOLCHAIN_ACTIVE)
    add_subdirectory(platform/ios)
    add_subdirectory(render/ios)

    if (ORCASLICER_BUILD_IOS_SMOKE_TARGET)
        add_library(orcaslicer_ios_smoke STATIC
            ios_smoke.cpp
        )
        target_link_libraries(orcaslicer_ios_smoke
            PRIVATE
                libslic3r
                orcaslicer_portability_api
                orcaslicer_platform_ios
                orcaslicer_render_ios_metal
        )
        target_include_directories(orcaslicer_ios_smoke
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/..
        )
        message(STATUS "Enabled iOS smoke target: orcaslicer_ios_smoke (libslic3r + portability iOS modules)")
    else ()
        message(STATUS "iOS smoke target disabled (ORCASLICER_BUILD_IOS_SMOKE_TARGET=OFF)")
    endif ()
else ()
    message(STATUS "Skipping iOS portability targets (non-iOS toolchain or portability disabled)")
endif ()
