# FindLibGit2
# -----------
#
# Find the libgit2 library.
#
# IMPORTED Targets
# ^^^^^^^^^^^^^^^^
#
# This module defines :prop_tgt:`IMPORTED` target ``LibGit2::LibGit2``, if
# libgit2 has been found.
#
# Result Variables
# ^^^^^^^^^^^^^^^^
#
# This module defines the following variables:
#
# ``LIBGIT2_FOUND``
#   True if libgit2 found.
#
# ``LIBGIT2_INCLUDE_DIRS``
#   where to find git2.h
#
# ``LIBGIT2_LIBRARIES``
#   List of libraries when using libgit2.
#
# ``LIBGIT2_VERSION_STRING``
#   The version of libgit2 found.

# Look for the header file.
find_path(LIBGIT2_INCLUDE_DIR NAMES git2.h)
mark_as_advanced(LIBGIT2_INCLUDE_DIR)

if(NOT LIBGIT2_LIBRARY)
  find_library(LIBGIT2_LIBRARY_RELEASE NAMES
      git2
      libgit2
  )
  mark_as_advanced(LIBGIT2_LIBRARY_RELEASE)

  find_library(LIBGIT2_LIBRARY_DEBUG NAMES
      git2d
      libgit2d
      git2
      libgit2
  )
  mark_as_advanced(LIBGIT2_LIBRARY_DEBUG)

  include(${CMAKE_CURRENT_LIST_DIR}/SelectLibraryConfigurations_SLIC3R.cmake)
  select_library_configurations_SLIC3R(LIBGIT2)
endif()

if(LIBGIT2_INCLUDE_DIR AND EXISTS "${LIBGIT2_INCLUDE_DIR}/git2/version.h")
  file(STRINGS "${LIBGIT2_INCLUDE_DIR}/git2/version.h" _libgit2_version_str
       REGEX "^#define[\t ]+LIBGIT2_VERSION[\t ]+\".*\"")
  string(REGEX REPLACE "^#define[\t ]+LIBGIT2_VERSION[\t ]+\"([^\"]*)\".*" "\\1"
         LIBGIT2_VERSION_STRING "${_libgit2_version_str}")
  unset(_libgit2_version_str)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/FindPackageHandleStandardArgs_SLIC3R.cmake)
FIND_PACKAGE_HANDLE_STANDARD_ARGS_SLIC3R(LibGit2
                                  REQUIRED_VARS LIBGIT2_LIBRARY LIBGIT2_INCLUDE_DIR
                                  VERSION_VAR LIBGIT2_VERSION_STRING)

if(LIBGIT2_FOUND)
  set(LIBGIT2_LIBRARIES ${LIBGIT2_LIBRARY})
  set(LIBGIT2_INCLUDE_DIRS ${LIBGIT2_INCLUDE_DIR})

  if(NOT TARGET LibGit2::LibGit2)
    add_library(LibGit2::LibGit2 UNKNOWN IMPORTED)
    set_target_properties(LibGit2::LibGit2 PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${LIBGIT2_INCLUDE_DIRS}")

    if(EXISTS "${LIBGIT2_LIBRARY}")
      set_target_properties(LibGit2::LibGit2 PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${LIBGIT2_LIBRARY}")
    endif()
    if(LIBGIT2_LIBRARY_RELEASE)
      set_property(TARGET LibGit2::LibGit2 APPEND PROPERTY
        IMPORTED_CONFIGURATIONS RELEASE)
      set_target_properties(LibGit2::LibGit2 PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION_RELEASE "${LIBGIT2_LIBRARY_RELEASE}")
    endif()
    if(LIBGIT2_LIBRARY_DEBUG)
      set_property(TARGET LibGit2::LibGit2 APPEND PROPERTY
        IMPORTED_CONFIGURATIONS DEBUG)
      set_target_properties(LibGit2::LibGit2 PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION_DEBUG "${LIBGIT2_LIBRARY_DEBUG}")
    endif()

    # Add platform-specific transitive dependencies required for static linking.
    if(WIN32)
      set_property(TARGET LibGit2::LibGit2 APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ws2_32 secur32 rpcrt4 crypt32 ole32)
    elseif(APPLE)
      set_property(TARGET LibGit2::LibGit2 APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "-framework Security" "-framework CoreFoundation")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set_property(TARGET LibGit2::LibGit2 APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES rt)
    endif()
  endif()
endif()
