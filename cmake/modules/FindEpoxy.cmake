#[=======================================================================[.rst:
FindEpoxy
---------

Find the libepoxy OpenGL function loader library.

Imported Targets
^^^^^^^^^^^^^^^^

``epoxy::epoxy``
  The libepoxy library (static).

Result Variables
^^^^^^^^^^^^^^^^

``EPOXY_FOUND``
  True if libepoxy was found.
``EPOXY_INCLUDE_DIRS``
  Include directories for libepoxy.
``EPOXY_LIBRARIES``
  Libraries to link against.

#]=======================================================================]

include(FindPackageHandleStandardArgs)

find_path(EPOXY_INCLUDE_DIR epoxy/gl.h)
mark_as_advanced(EPOXY_INCLUDE_DIR)

find_library(EPOXY_LIBRARY
    NAMES epoxy
    PATH_SUFFIXES lib lib64
)
mark_as_advanced(EPOXY_LIBRARY)

# Read version from epoxy/gl.h if available
if (EXISTS "${EPOXY_INCLUDE_DIR}/epoxy/gl.h")
    file(STRINGS "${EPOXY_INCLUDE_DIR}/epoxy/gl.h" _epoxy_version_line
         REGEX "^#define[ \t]+EPOXY_GL_VERSION[ \t]+")
    if (_epoxy_version_line)
        string(REGEX REPLACE ".*EPOXY_GL_VERSION[ \t]+([0-9]+).*" "\\1" EPOXY_VERSION "${_epoxy_version_line}")
    endif ()
endif ()

find_package_handle_standard_args(Epoxy
    REQUIRED_VARS EPOXY_LIBRARY EPOXY_INCLUDE_DIR
    VERSION_VAR EPOXY_VERSION
)

if (EPOXY_FOUND)
    set(EPOXY_INCLUDE_DIRS ${EPOXY_INCLUDE_DIR})
    set(EPOXY_LIBRARIES    ${EPOXY_LIBRARY})

    if (NOT TARGET epoxy::epoxy)
        add_library(epoxy::epoxy UNKNOWN IMPORTED)
        set_target_properties(epoxy::epoxy PROPERTIES
            IMPORTED_LOCATION             "${EPOXY_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${EPOXY_INCLUDE_DIR}"
        )
        # libepoxy uses dlopen() to load GL libraries at runtime
        if (UNIX AND NOT APPLE)
            set_property(TARGET epoxy::epoxy APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES ${CMAKE_DL_LIBS}
            )
        endif ()
    endif ()
endif ()
