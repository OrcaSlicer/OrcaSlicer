set(_q "")
if(${CMAKE_FIND_PACKAGE_NAME}_FIND_QUIETLY)
    set(_q QUIET)
    set(_quietly TRUE)
endif()
find_package(${CMAKE_FIND_PACKAGE_NAME} ${${CMAKE_FIND_PACKAGE_NAME}_FIND_VERSION} CONFIG ${_q})

if (NOT ${CMAKE_FIND_PACKAGE_NAME}_FOUND)
    include(CheckIncludeFileCXX)

    if (_quietly)
        set(CMAKE_REQUIRED_QUIET ON)
    endif()
    CHECK_INCLUDE_FILE_CXX("draco/draco_features.h" HAVE_DRACO_H)

    if (NOT HAVE_DRACO_H)
        if (${CMAKE_FIND_PACKAGE_NAME}_FIND_REQUIRED)
            message(FATAL_ERROR "Draco library not found. Please install the dependency.")
        elseif(NOT _quietly)
            message(WARNING "Draco library not found.")
        endif()
    endif ()

    find_path(DRACO_INCLUDE_DIR draco/draco_features.h)
    if (NOT TARGET draco::draco)
        add_library(draco::draco INTERFACE IMPORTED)
        if (DRACO_INCLUDE_DIR)
            set_target_properties(draco::draco PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${DRACO_INCLUDE_DIR}"
            )
        endif()
    endif()
endif()
