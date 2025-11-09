find_path(LIBNOISE_INCLUDE_DIR libnoise/noise.h)
find_library(LIBNOISE_LIBRARY_RELEASE NAMES libnoise libnoise_static liblibnoise_static)
find_library(LIBNOISE_LIBRARY_DEBUG NAMES libnoised libnoise_staticd liblibnoise_staticd)

set(libnoise_LIB_FOUND FALSE)
if (LIBNOISE_LIBRARY_RELEASE OR LIBNOISE_LIBRARY_DEBUG)
    set(libnoise_LIB_FOUND TRUE)
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libnoise DEFAULT_MSG
    libnoise_LIB_FOUND
    LIBNOISE_INCLUDE_DIR
)

if(libnoise_FOUND)
    add_library(noise::noise STATIC IMPORTED)

    set_target_properties(noise::noise PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LIBNOISE_INCLUDE_DIR}"
    )
    if (NOT libnoise_FIND_QUIETLY)
        message(STATUS "Found libnoise include directory: ${LIBNOISE_INCLUDE_DIR}")
        if (LIBNOISE_LIBRARY_RELEASE)
            message(STATUS "Found libnoise RELEASE library: ${LIBNOISE_LIBRARY_RELEASE}")
        endif ()
        if (LIBNOISE_LIBRARY_DEBUG)
            message(STATUS "Found libnoise DEBUG library: ${LIBNOISE_LIBRARY_DEBUG}")
        endif ()
    endif ()

    if (LIBNOISE_LIBRARY_RELEASE)
        set_property(TARGET noise::noise APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
        set_target_properties(noise::noise PROPERTIES IMPORTED_LOCATION_RELEASE ${LIBNOISE_LIBRARY_RELEASE})
    endif ()

    if (LIBNOISE_LIBRARY_DEBUG)
        set_property(TARGET noise::noise APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
        set_target_properties(noise::noise PROPERTIES IMPORTED_LOCATION_DEBUG ${LIBNOISE_LIBRARY_DEBUG})
    endif ()
endif()