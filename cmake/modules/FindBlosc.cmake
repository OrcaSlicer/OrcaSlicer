# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0
#
#[=======================================================================[.rst:

FindBlosc
---------

Find Blosc include dirs and libraries

Use this module by invoking find_package with the form::

  find_package(Blosc
    [version] [EXACT]      # Minimum or EXACT version e.g. 1.5.0
    [REQUIRED]             # Fail with error if Blosc is not found
    )

IMPORTED Targets
^^^^^^^^^^^^^^^^

``Blosc::blosc``
  This module defines IMPORTED target Blosc::Blosc, if Blosc has been found.

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables:

``Blosc_FOUND``
  True if the system has the Blosc library.
``Blosc_VERSION``
  The version of the Blosc library which was found.
``Blosc_INCLUDE_DIRS``
  Include directories needed to use Blosc.
``Blosc_RELEASE_LIBRARIES``
  Libraries needed to link to the release version of Blosc.
``Blosc_RELEASE_LIBRARY_DIRS``
  Blosc release library directories.
``Blosc_DEBUG_LIBRARIES``
  Libraries needed to link to the debug version of Blosc.
``Blosc_DEBUG_LIBRARY_DIRS``
  Blosc debug library directories.

Deprecated - use [RELEASE|DEBUG] variants:

``Blosc_LIBRARIES``
  Libraries needed to link to Blosc.
``Blosc_LIBRARY_DIRS``
  Blosc library directories.

Cache Variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``Blosc_INCLUDE_DIR``
  The directory containing ``blosc.h``.
``Blosc_LIBRARY``
  The path to the Blosc library. may include target_link_libraries() debug/optimized keywords
``Blosc_LIBRARY_RELEASE``
  The path to the Blosc release library.
``Blosc_LIBRARY_DEBUG``
  The path to the Blosc debug library.

Hints
^^^^^

Instead of explicitly setting the cache variables, the following variables
may be provided to tell this module where to look.

``Blosc_ROOT``
  Preferred installation prefix.
``BLOSC_INCLUDEDIR``
  Preferred include directory e.g. <prefix>/include
``BLOSC_LIBRARYDIR``
  Preferred library directory e.g. <prefix>/lib
``BLOSC_DEBUG_SUFFIX``
  Suffix of the debug version of blosc. Defaults to "_d", OR the empty string for VCPKG_TOOLCHAIN
``SYSTEM_LIBRARY_PATHS``
  Global list of library paths intended to be searched by and find_xxx call
``BLOSC_USE_STATIC_LIBS``
  Only search for static blosc libraries
``BLOSC_USE_EXTERNAL_SOURCES``
  Set to ON if Blosc has been built using external sources for LZ4, snappy,
  zlib and zstd. Default is OFF.
``DISABLE_CMAKE_SEARCH_PATHS``
  Disable CMakes default search paths for find_xxx calls in this module

#]=======================================================================]

cmake_minimum_required(VERSION 3.20)
include(GNUInstallDirs)

mark_as_advanced(
  Blosc_INCLUDE_DIR
  Blosc_LIBRARY
)

set(_FIND_BLOSC_ADDITIONAL_OPTIONS "")
if(DISABLE_CMAKE_SEARCH_PATHS)
  set(_FIND_BLOSC_ADDITIONAL_OPTIONS NO_DEFAULT_PATH)
endif()

# Set _BLOSC_ROOT based on a user provided root var. Xxx_ROOT and ENV{Xxx_ROOT}
# are prioritised over the legacy capitalized XXX_ROOT variables for matching
# CMake 3.12 behaviour
# @todo  deprecate -D and ENV BLOSC_ROOT from CMake 3.12
if(Blosc_ROOT)
  set(_BLOSC_ROOT ${Blosc_ROOT})
elseif(DEFINED ENV{Blosc_ROOT})
  set(_BLOSC_ROOT $ENV{Blosc_ROOT})
elseif(BLOSC_ROOT)
  set(_BLOSC_ROOT ${BLOSC_ROOT})
elseif(DEFINED ENV{BLOSC_ROOT})
  set(_BLOSC_ROOT $ENV{BLOSC_ROOT})
endif()

# Additionally try and use pkconfig to find blosc
if(USE_PKGCONFIG)
  if(NOT DEFINED PKG_CONFIG_FOUND)
    find_package(PkgConfig)
  endif()
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_Blosc QUIET blosc)
  endif()
endif()

# ------------------------------------------------------------------------
#  Search for blosc include DIR
# ------------------------------------------------------------------------

set(_BLOSC_INCLUDE_SEARCH_DIRS "")
list(APPEND _BLOSC_INCLUDE_SEARCH_DIRS
  ${BLOSC_INCLUDEDIR}
  ${_BLOSC_ROOT}
  ${PC_Blosc_INCLUDE_DIRS}
  ${SYSTEM_LIBRARY_PATHS}
)

# Look for a standard blosc header file.
find_path(Blosc_INCLUDE_DIR blosc.h
  ${_FIND_BLOSC_ADDITIONAL_OPTIONS}
  PATHS ${_BLOSC_INCLUDE_SEARCH_DIRS}
  PATH_SUFFIXES ${CMAKE_INSTALL_INCLUDEDIR} include
)

if(EXISTS "${Blosc_INCLUDE_DIR}/blosc.h")
  file(STRINGS "${Blosc_INCLUDE_DIR}/blosc.h"
    _blosc_version_major_string REGEX "#define BLOSC_VERSION_MAJOR +[0-9]+ "
  )
  string(REGEX REPLACE "#define BLOSC_VERSION_MAJOR +([0-9]+).*$" "\\1"
    _blosc_version_major_string "${_blosc_version_major_string}"
  )
  string(STRIP "${_blosc_version_major_string}" Blosc_VERSION_MAJOR)

  file(STRINGS "${Blosc_INCLUDE_DIR}/blosc.h"
     _blosc_version_minor_string REGEX "#define BLOSC_VERSION_MINOR +[0-9]+ "
  )
  string(REGEX REPLACE "#define BLOSC_VERSION_MINOR +([0-9]+).*$" "\\1"
    _blosc_version_minor_string "${_blosc_version_minor_string}"
  )
  string(STRIP "${_blosc_version_minor_string}" Blosc_VERSION_MINOR)

  file(STRINGS "${Blosc_INCLUDE_DIR}/blosc.h"
     _blosc_version_release_string REGEX "#define BLOSC_VERSION_RELEASE +[0-9]+ "
  )
  string(REGEX REPLACE "#define BLOSC_VERSION_RELEASE +([0-9]+).*$" "\\1"
    _blosc_version_release_string "${_blosc_version_release_string}"
  )
  string(STRIP "${_blosc_version_release_string}" Blosc_VERSION_RELEASE)

  unset(_blosc_version_major_string)
  unset(_blosc_version_minor_string)
  unset(_blosc_version_release_string)

  set(Blosc_VERSION ${Blosc_VERSION_MAJOR}.${Blosc_VERSION_MINOR}.${Blosc_VERSION_RELEASE})
endif()

# ------------------------------------------------------------------------
#  Search for blosc lib DIR
# ------------------------------------------------------------------------

set(_BLOSC_LIBRARYDIR_SEARCH_DIRS "")
list(APPEND _BLOSC_LIBRARYDIR_SEARCH_DIRS
  ${BLOSC_LIBRARYDIR}
  ${_BLOSC_ROOT}
  ${PC_Blosc_LIBRARY_DIRS}
  ${SYSTEM_LIBRARY_PATHS}
)

set(Blosc_LIB_COMPONENTS "")
# NOTE: Search for debug version first (see vcpkg hack)
list(APPEND BLOSC_BUILD_TYPES DEBUG RELEASE)

foreach(BUILD_TYPE ${BLOSC_BUILD_TYPES})
  set(_BLOSC_LIB_NAME blosc)

  set(_BLOSC_CMAKE_IGNORE_PATH ${CMAKE_IGNORE_PATH})
  if(VCPKG_TOOLCHAIN)
    # Blosc is installed very strangely in VCPKG (debug/release libs have the
    # same name, static build uses external deps, dll doesn't) and blosc itself
    # comes with almost zero downstream CMake support for us to detect settings.
    # We should not support external package managers in our own modules like
    # this, but there doesn't seem to be a work around
    if(NOT DEFINED BLOSC_DEBUG_SUFFIX)
      set(BLOSC_DEBUG_SUFFIX "")
    endif()
    if(BUILD_TYPE STREQUAL RELEASE)
      if(EXISTS ${Blosc_LIBRARY_DEBUG})
        get_filename_component(_BLOSC_DEBUG_DIR ${Blosc_LIBRARY_DEBUG} DIRECTORY)
        list(APPEND CMAKE_IGNORE_PATH ${_BLOSC_DEBUG_DIR})
      endif()
    endif()
  endif()

  if(BUILD_TYPE STREQUAL DEBUG)
    if(NOT DEFINED BLOSC_DEBUG_SUFFIX)
      set(BLOSC_DEBUG_SUFFIX _d)
    endif()
    set(_BLOSC_LIB_NAME "${_BLOSC_LIB_NAME}${BLOSC_DEBUG_SUFFIX}")
  endif()

  find_library(Blosc_LIBRARY_${BUILD_TYPE} ${_BLOSC_LIB_NAME}
    ${_FIND_BLOSC_ADDITIONAL_OPTIONS}
    PATHS ${_BLOSC_LIBRARYDIR_SEARCH_DIRS}
    PATH_SUFFIXES ${CMAKE_INSTALL_LIBDIR} lib64 lib
  )

  list(APPEND Blosc_LIB_COMPONENTS ${Blosc_LIBRARY_${BUILD_TYPE}})
  set(CMAKE_IGNORE_PATH ${_BLOSC_CMAKE_IGNORE_PATH})
endforeach()

if(Blosc_LIBRARY_DEBUG AND Blosc_LIBRARY_RELEASE)
  # if the generator is multi-config or if CMAKE_BUILD_TYPE is set for
  # single-config generators, set optimized and debug libraries
  get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(_isMultiConfig OR CMAKE_BUILD_TYPE)
    set(Blosc_LIBRARY optimized ${Blosc_LIBRARY_RELEASE} debug ${Blosc_LIBRARY_DEBUG})
  else()
    # For single-config generators where CMAKE_BUILD_TYPE has no value,
    # just use the release libraries
    set(Blosc_LIBRARY ${Blosc_LIBRARY_RELEASE})
  endif()
  # FIXME: This probably should be set for both cases
  set(Blosc_LIBRARIES optimized ${Blosc_LIBRARY_RELEASE} debug ${Blosc_LIBRARY_DEBUG})
endif()

# if only the release version was found, set the debug variable also to the release version
if(Blosc_LIBRARY_RELEASE AND NOT Blosc_LIBRARY_DEBUG)
  set(Blosc_LIBRARY_DEBUG ${Blosc_LIBRARY_RELEASE})
  set(Blosc_LIBRARY       ${Blosc_LIBRARY_RELEASE})
  set(Blosc_LIBRARIES     ${Blosc_LIBRARY_RELEASE})
endif()

# if only the debug version was found, set the release variable also to the debug version
if(Blosc_LIBRARY_DEBUG AND NOT Blosc_LIBRARY_RELEASE)
  set(Blosc_LIBRARY_RELEASE ${Blosc_LIBRARY_DEBUG})
  set(Blosc_LIBRARY         ${Blosc_LIBRARY_DEBUG})
  set(Blosc_LIBRARIES       ${Blosc_LIBRARY_DEBUG})
endif()

# If the debug & release library ends up being the same, omit the keywords
if("${Blosc_LIBRARY_RELEASE}" STREQUAL "${Blosc_LIBRARY_DEBUG}")
  set(Blosc_LIBRARY   ${Blosc_LIBRARY_RELEASE} )
  set(Blosc_LIBRARIES ${Blosc_LIBRARY_RELEASE} )
endif()

if(Blosc_LIBRARY)
  set(Blosc_FOUND TRUE)
else()
  set(Blosc_FOUND FALSE)
endif()

# ------------------------------------------------------------------------
#  Cache and set Blosc_FOUND
# ------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Blosc
  FOUND_VAR Blosc_FOUND
  REQUIRED_VARS
    Blosc_LIBRARY
    Blosc_INCLUDE_DIR
  VERSION_VAR Blosc_VERSION
)

if(NOT Blosc_FOUND)
  if(Blosc_FIND_REQUIRED)
    message(FATAL_ERROR "Unable to find Blosc")
  endif()
  return()
endif()

# Partition release/debug lib vars

set(Blosc_RELEASE_LIBRARIES ${Blosc_LIBRARY_RELEASE})
get_filename_component(Blosc_RELEASE_LIBRARY_DIRS ${Blosc_RELEASE_LIBRARIES} DIRECTORY)
set(Blosc_DEBUG_LIBRARIES ${Blosc_LIBRARY_DEBUG})
get_filename_component(Blosc_DEBUG_LIBRARY_DIRS ${Blosc_DEBUG_LIBRARIES} DIRECTORY)
set(Blosc_LIBRARIES ${Blosc_RELEASE_LIBRARIES})
set(Blosc_LIBRARY_DIRS ${Blosc_RELEASE_LIBRARY_DIRS})
set(Blosc_INCLUDE_DIRS ${Blosc_INCLUDE_DIR})
set(Blosc_INCLUDE_DIRS ${Blosc_INCLUDE_DIR})

# Configure lib type. If XXX_USE_STATIC_LIBS, we always assume a static
# lib is in use. If win32, we can't mark the import .libs as shared, so
# these are always marked as UNKNOWN. Otherwise, infer from extension.
set(BLOSC_LIB_TYPE UNKNOWN)
if(BLOSC_USE_STATIC_LIBS)
  set(BLOSC_LIB_TYPE STATIC)
elseif(UNIX)
  get_filename_component(_BLOSC_EXT ${Blosc_LIBRARY_RELEASE} EXT)
  if(_BLOSC_EXT STREQUAL ".a")
    set(BLOSC_LIB_TYPE STATIC)
  elseif(_BLOSC_EXT STREQUAL ".so" OR
         _BLOSC_EXT STREQUAL ".dylib")
    set(BLOSC_LIB_TYPE SHARED)
  endif()
endif()

get_filename_component(Blosc_LIBRARY_DIRS ${Blosc_LIBRARY_RELEASE} DIRECTORY)

if(NOT TARGET Blosc::blosc)
  add_library(Blosc::blosc ${BLOSC_LIB_TYPE} IMPORTED)
  set_target_properties(Blosc::blosc PROPERTIES
    INTERFACE_COMPILE_OPTIONS "${PC_Blosc_CFLAGS_OTHER}"
    INTERFACE_INCLUDE_DIRECTORIES "${Blosc_INCLUDE_DIRS}")

  # Standard location
  set_target_properties(Blosc::blosc PROPERTIES
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    IMPORTED_LOCATION "${Blosc_LIBRARY}")

  # Release location
  if(EXISTS "${Blosc_LIBRARY_RELEASE}")
    set_property(TARGET Blosc::blosc APPEND PROPERTY
      IMPORTED_CONFIGURATIONS RELEASE)
    set_target_properties(Blosc::blosc PROPERTIES
      IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
      IMPORTED_LOCATION_RELEASE "${Blosc_LIBRARY_RELEASE}")
  endif()

  # Debug location
  if(EXISTS "${Blosc_LIBRARY_DEBUG}")
    set_property(TARGET Blosc::blosc APPEND PROPERTY
      IMPORTED_CONFIGURATIONS DEBUG)
    set_target_properties(Blosc::blosc PROPERTIES
      IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
      IMPORTED_LOCATION_DEBUG "${Blosc_LIBRARY_DEBUG}")
  endif()

  # Blosc may optionally be compiled with external sources for
  # lz4, snappy and zlib . Add them as interface libs if requested
  # (there doesn't seem to be a way to figure this out automatically).
  # We assume they live along side blosc
  if(BLOSC_USE_EXTERNAL_SOURCES)
    set_target_properties(Blosc::blosc PROPERTIES
      INTERFACE_LINK_DIRECTORIES
         "\$<\$<CONFIG:Release>:${Blosc_RELEASE_LIBRARY_DIRS}>;\$<\$<CONFIG:Debug>:${Blosc_DEBUG_LIBRARY_DIRS}>")

    set(BLOSC_EXTERNAL_LIBRARIES lz4 snappy zlib zstd)

    foreach(BLOSC_EXTERNAL_LIB ${BLOSC_EXTERNAL_LIBRARIES})

      foreach(BUILD_TYPE ${BLOSC_BUILD_TYPES})
        set(_BLOSC_CMAKE_IGNORE_PATH ${CMAKE_IGNORE_PATH})

        if(VCPKG_TOOLCHAIN)
          if(BUILD_TYPE STREQUAL RELEASE)
            list(APPEND CMAKE_IGNORE_PATH ${Blosc_DEBUG_LIBRARY_DIRS})
          else()
            list(APPEND CMAKE_IGNORE_PATH ${Blosc_RELEASE_LIBRARY_DIRS})
          endif()
        endif()

        find_library(${BLOSC_EXTERNAL_LIB}_LIBRARY_${BUILD_TYPE} ${BLOSC_EXTERNAL_LIB}
          ${_FIND_BLOSC_ADDITIONAL_OPTIONS}
          PATHS ${_BLOSC_LIBRARYDIR_SEARCH_DIRS}
          PATH_SUFFIXES ${CMAKE_INSTALL_LIBDIR} lib64 lib)

        if(NOT ${BLOSC_EXTERNAL_LIB}_LIBRARY_${BUILD_TYPE})
          if(BUILD_TYPE STREQUAL DEBUG)
            find_library(${BLOSC_EXTERNAL_LIB}_LIBRARY_${BUILD_TYPE} "${BLOSC_EXTERNAL_LIB}d"
              ${_FIND_BLOSC_ADDITIONAL_OPTIONS}
              PATHS ${_BLOSC_LIBRARYDIR_SEARCH_DIRS}
              PATH_SUFFIXES ${CMAKE_INSTALL_LIBDIR} lib64 lib)
          endif()
        endif()

        set(CMAKE_IGNORE_PATH ${_BLOSC_CMAKE_IGNORE_PATH})
      endforeach()

      if(${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG AND ${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE)
        # if the generator is multi-config or if CMAKE_BUILD_TYPE is set for
        # single-config generators, set optimized and debug libraries
        get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(_isMultiConfig OR CMAKE_BUILD_TYPE)
          set(${BLOSC_EXTERNAL_LIB}_LIBRARY optimized ${${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE} debug ${${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG})
        else()
          # For single-config generators where CMAKE_BUILD_TYPE has no value,
          # just use the release libraries
          set(${BLOSC_EXTERNAL_LIB}_LIBRARY ${${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE})
        endif()
        # FIXME: This probably should be set for both cases
        set(${BLOSC_EXTERNAL_LIB}_LIBRARIES optimized ${${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE} debug ${${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG})
      endif()

      # if only the release version was found, set the debug variable also to the release version
      if(${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE AND NOT ${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG)
        set(${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG ${${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE})
        set(${BLOSC_EXTERNAL_LIB}_LIBRARY       ${${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE})
        set(${BLOSC_EXTERNAL_LIB}_LIBRARIES     ${${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE})
      endif()

      # if only the debug version was found, set the release variable also to the debug version
      if(${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG AND NOT ${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE)
        set(${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE ${${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG})
        set(${BLOSC_EXTERNAL_LIB}_LIBRARY         ${${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG})
        set(${BLOSC_EXTERNAL_LIB}_LIBRARIES       ${${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG})
      endif()

      target_link_libraries(Blosc::blosc INTERFACE
        $<$<CONFIG:Release>:${${BLOSC_EXTERNAL_LIB}_LIBRARY_RELEASE}>
        $<$<CONFIG:Debug>:${${BLOSC_EXTERNAL_LIB}_LIBRARY_DEBUG}>)

    endforeach()
  endif()

endif()
