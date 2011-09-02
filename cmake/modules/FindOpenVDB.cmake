# Copyright (c) DreamWorks Animation LLC
#
# All rights reserved. This software is distributed under the
# Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
#
# Redistributions of source code must retain the above copyright
# and license notice and the following restrictions and disclaimer.
#
# *     Neither the name of DreamWorks Animation nor the names of
# its contributors may be used to endorse or promote products derived
# from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
# LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
#
#[=======================================================================[.rst:

FindOpenVDB
-----------

Find OpenVDB include dirs, libraries and settings

Use this module by invoking find_package with the form::

  find_package(OpenVDB
    [version] [EXACT]      # Minimum or EXACT version
    [REQUIRED]             # Fail with error if OpenVDB is not found
    [COMPONENTS <libs>...] # OpenVDB libraries by their canonical name
                           # e.g. "openvdb" for "libopenvdb"
    )

IMPORTED Targets
^^^^^^^^^^^^^^^^

``OpenVDB::openvdb``
  The core openvdb library target.

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables:

``OpenVDB_FOUND``
  True if the system has the OpenVDB library.
``OpenVDB_VERSION``
  The version of the OpenVDB library which was found.
``OpenVDB_INCLUDE_DIRS``
  Include directories needed to use OpenVDB.
``OpenVDB_LIBRARIES``
  Libraries needed to link to OpenVDB.
``OpenVDB_LIBRARY_DIRS``
  OpenVDB library directories.
``OpenVDB_DEFINITIONS``
  Definitions to use when compiling code that uses OpenVDB.
``OpenVDB_{COMPONENT}_FOUND``
  True if the system has the named OpenVDB component.
``OpenVDB_USES_BLOSC``
  True if the OpenVDB Library has been built with blosc support
``OpenVDB_USES_LOG4CPLUS``
  True if the OpenVDB Library has been built with log4cplus support
``OpenVDB_USES_EXR``
  True if the OpenVDB Library has been built with openexr support
``OpenVDB_ABI``
  Set if this module was able to determine the ABI number the located
  OpenVDB Library was built against. Unset otherwise.

Cache Variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``OpenVDB_INCLUDE_DIR``
  The directory containing ``openvdb/version.h``.
``OpenVDB_{COMPONENT}_LIBRARY``
  Individual component libraries for OpenVDB

Hints
^^^^^

Instead of explicitly setting the cache variables, the following variables
may be provided to tell this module where to look.

``OPENVDB_ROOT``
  Preferred installation prefix.
``OPENVDB_INCLUDEDIR``
  Preferred include directory e.g. <prefix>/include
``OPENVDB_LIBRARYDIR``
  Preferred library directory e.g. <prefix>/lib
``SYSTEM_LIBRARY_PATHS``
  Paths appended to all include and lib searches.

#]=======================================================================]

# If an explicit openvdb module path was specified, that will be used
if (OPENVDB_FIND_MODULE_PATH)
  set(_module_path_bak ${CMAKE_MODULE_PATH})
  set(CMAKE_MODULE_PATH ${OPENVDB_FIND_MODULE_PATH})
  find_package(
    OpenVDB ${OpenVDB_FIND_VERSION} QUIET
    COMPONENTS
      ${OpenVDB_FIND_COMPONENTS}
  )

  set(CMAKE_MODULE_PATH ${_module_path_bak})
  if (OpenVDB_FOUND)
    return()
  endif ()

  if (NOT OpenVDB_FIND_QUIETLY)
    message(STATUS "Using bundled find module for OpenVDB")
  endif ()
endif ()
# ###########################################################################

cmake_minimum_required(VERSION 3.13)
# Monitoring <PackageName>_ROOT variables
if(POLICY CMP0074)
  cmake_policy(SET CMP0074 NEW)
endif()

if(OpenVDB_FIND_QUIETLY)
  set (_quiet "QUIET")
else()
  set (_quiet "")
endif()

if(OpenVDB_FIND_REQUIRED)
  set (_required "REQUIRED")
else()
  set (_required "")
endif()

# Include utility functions for version information
include(${CMAKE_CURRENT_LIST_DIR}/OpenVDBUtils.cmake)

mark_as_advanced(
  OpenVDB_INCLUDE_DIR
  OpenVDB_LIBRARY
)

set(_OPENVDB_COMPONENT_LIST
  openvdb
)

if(OpenVDB_FIND_COMPONENTS)
  set(OPENVDB_COMPONENTS_PROVIDED TRUE)
  set(_IGNORED_COMPONENTS "")
  foreach(COMPONENT ${OpenVDB_FIND_COMPONENTS})
    if(NOT ${COMPONENT} IN_LIST _OPENVDB_COMPONENT_LIST)
      list(APPEND _IGNORED_COMPONENTS ${COMPONENT})
    endif()
  endforeach()

  if(_IGNORED_COMPONENTS)
    message(STATUS "Ignoring unknown components of OpenVDB:")
    foreach(COMPONENT ${_IGNORED_COMPONENTS})
      message(STATUS "  ${COMPONENT}")
    endforeach()
    list(REMOVE_ITEM OpenVDB_FIND_COMPONENTS ${_IGNORED_COMPONENTS})
  endif()
else()
  set(OPENVDB_COMPONENTS_PROVIDED FALSE)
  set(OpenVDB_FIND_COMPONENTS ${_OPENVDB_COMPONENT_LIST})
endif()

# Append OPENVDB_ROOT or $ENV{OPENVDB_ROOT} if set (prioritize the direct cmake var)
set(_OPENVDB_ROOT_SEARCH_DIR "")

# Additionally try and use pkconfig to find OpenVDB

find_package(PkgConfig ${_quiet} )
pkg_check_modules(PC_OpenVDB QUIET OpenVDB)

# ------------------------------------------------------------------------
#  Search for OpenVDB include DIR
# ------------------------------------------------------------------------

set(_OPENVDB_INCLUDE_SEARCH_DIRS "")
list(APPEND _OPENVDB_INCLUDE_SEARCH_DIRS
  ${OPENVDB_INCLUDEDIR}
  ${_OPENVDB_ROOT_SEARCH_DIR}
  ${PC_OpenVDB_INCLUDE_DIRS}
  ${SYSTEM_LIBRARY_PATHS}
)

# Look for a standard OpenVDB header file.
find_path(OpenVDB_INCLUDE_DIR openvdb/version.h
  PATHS ${_OPENVDB_INCLUDE_SEARCH_DIRS}
  PATH_SUFFIXES include
)

OPENVDB_VERSION_FROM_HEADER("${OpenVDB_INCLUDE_DIR}/openvdb/version.h"
  VERSION OpenVDB_VERSION
  MAJOR   OpenVDB_MAJOR_VERSION
  MINOR   OpenVDB_MINOR_VERSION
  PATCH   OpenVDB_PATCH_VERSION
)

# ------------------------------------------------------------------------
#  Search for OPENVDB lib DIR
# ------------------------------------------------------------------------

set(_OPENVDB_LIBRARYDIR_SEARCH_DIRS "")

# Append to _OPENVDB_LIBRARYDIR_SEARCH_DIRS in priority order

list(APPEND _OPENVDB_LIBRARYDIR_SEARCH_DIRS
  ${OPENVDB_LIBRARYDIR}
  ${_OPENVDB_ROOT_SEARCH_DIR}
  ${PC_OpenVDB_LIBRARY_DIRS}
  ${SYSTEM_LIBRARY_PATHS}
)

# Build suffix directories

set(OPENVDB_PATH_SUFFIXES
  lib64
  lib
)

# Static library setup
if(UNIX AND OPENVDB_USE_STATIC_LIBS)
  set(_OPENVDB_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

set(OpenVDB_LIB_COMPONENTS "")
set(OpenVDB_DEBUG_SUFFIX "d" CACHE STRING "Suffix for the debug libraries")

# get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_is_multi FALSE)

foreach(COMPONENT ${OpenVDB_FIND_COMPONENTS})
  set(LIB_NAME ${COMPONENT})

  find_library(OpenVDB_${COMPONENT}_LIBRARY_RELEASE ${LIB_NAME} lib${LIB_NAME}
    PATHS ${_OPENVDB_LIBRARYDIR_SEARCH_DIRS}
    PATH_SUFFIXES ${OPENVDB_PATH_SUFFIXES}
  )

  find_library(OpenVDB_${COMPONENT}_LIBRARY_DEBUG ${LIB_NAME}${OpenVDB_DEBUG_SUFFIX} lib${LIB_NAME}${OpenVDB_DEBUG_SUFFIX}
    PATHS ${_OPENVDB_LIBRARYDIR_SEARCH_DIRS}
    PATH_SUFFIXES ${OPENVDB_PATH_SUFFIXES}
  )

  if (_is_multi)
    list(APPEND OpenVDB_LIB_COMPONENTS ${OpenVDB_${COMPONENT}_LIBRARY_RELEASE})
    if (OpenVDB_${COMPONENT}_LIBRARY_DEBUG)
      list(APPEND OpenVDB_LIB_COMPONENTS ${OpenVDB_${COMPONENT}_LIBRARY_DEBUG})
    endif ()

    list(FIND CMAKE_CONFIGURATION_TYPES "Debug" _has_debug)
    
    if(OpenVDB_${COMPONENT}_LIBRARY_RELEASE AND (NOT MSVC OR _has_debug LESS 0 OR OpenVDB_${COMPONENT}_LIBRARY_DEBUG))
      set(OpenVDB_${COMPONENT}_FOUND TRUE)
    else()
      set(OpenVDB_${COMPONENT}_FOUND FALSE)
    endif()

    set(OpenVDB_${COMPONENT}_LIBRARY ${OpenVDB_${COMPONENT}_LIBRARY_RELEASE})
  else ()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _BUILD_TYPE)

    set(OpenVDB_${COMPONENT}_LIBRARY ${OpenVDB_${COMPONENT}_LIBRARY_${_BUILD_TYPE}})

    if (NOT OpenVDB_${COMPONENT}_LIBRARY)
      set(OpenVDB_${COMPONENT}_LIBRARY ${OpenVDB_${COMPONENT}_LIBRARY_RELEASE})
    endif ()

    list(APPEND OpenVDB_LIB_COMPONENTS ${OpenVDB_${COMPONENT}_LIBRARY})

    if(OpenVDB_${COMPONENT}_LIBRARY)
      set(OpenVDB_${COMPONENT}_FOUND TRUE)
    else()
      set(OpenVDB_${COMPONENT}_FOUND FALSE)
    endif()
  endif ()

endforeach()

if(UNIX AND OPENVDB_USE_STATIC_LIBS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_OPENVDB_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES})
  unset(_OPENVDB_ORIG_CMAKE_FIND_LIBRARY_SUFFIXES)
endif()

# ------------------------------------------------------------------------
#  Cache and set OPENVDB_FOUND
# ------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenVDB
  FOUND_VAR OpenVDB_FOUND
  REQUIRED_VARS
    OpenVDB_INCLUDE_DIR
    OpenVDB_LIB_COMPONENTS
  VERSION_VAR OpenVDB_VERSION
  HANDLE_COMPONENTS
)

# ------------------------------------------------------------------------
#  Determine ABI number
# ------------------------------------------------------------------------

# Set the ABI number the library was built against. Uses vdb_print
find_program(OPENVDB_PRINT vdb_print PATHS ${OpenVDB_INCLUDE_DIR} )

OPENVDB_ABI_VERSION_FROM_PRINT(
  "${OPENVDB_PRINT}"
  ABI OpenVDB_ABI
)

if(NOT OpenVDB_FIND_QUIETLY)
  if(NOT OpenVDB_ABI)
    message(WARNING "Unable to determine OpenVDB ABI version from OpenVDB "
      "installation. The library major version \"${OpenVDB_MAJOR_VERSION}\" "
      "will be inferred. If this is not correct, use "
      "add_definitions(-DOPENVDB_ABI_VERSION_NUMBER=N)"
    )
  else()
    message(STATUS "OpenVDB ABI Version: ${OpenVDB_ABI}")
  endif()
endif()

# ------------------------------------------------------------------------
#  Handle OpenVDB dependencies
# ------------------------------------------------------------------------

# Add standard dependencies

macro(just_fail msg)
  set(OpenVDB_FOUND FALSE)
  if(OpenVDB_FIND_REQUIRED)
    message(FATAL_ERROR ${msg})
  elseif(NOT OpenVDB_FIND_QUIETLY)
    message(WARNING ${msg})
  endif()
  return()
endmacro()

find_package(IlmBase QUIET)
if(NOT IlmBase_FOUND)
  pkg_check_modules(IlmBase QUIET IlmBase)
endif()
if (IlmBase_FOUND AND NOT TARGET IlmBase::Half)
  message(STATUS "Falling back to IlmBase found by pkg-config...")

  find_library(IlmHalf_LIBRARY NAMES Half)
  if(IlmHalf_LIBRARY-NOTFOUND OR NOT IlmBase_INCLUDE_DIRS)
    just_fail("IlmBase::Half can not be found!")
  endif()
  
  add_library(IlmBase::Half UNKNOWN IMPORTED)
  set_target_properties(IlmBase::Half PROPERTIES
    IMPORTED_LOCATION "${IlmHalf_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${IlmBase_INCLUDE_DIRS}")
elseif(NOT IlmBase_FOUND)
  just_fail("IlmBase::Half can not be found!")
endif()
find_package(TBB ${_quiet} ${_required} COMPONENTS tbb)
find_package(ZLIB ${_quiet} ${_required})
find_package(Boost ${_quiet} ${_required} COMPONENTS iostreams system )

# Use GetPrerequisites to see which libraries this OpenVDB lib has linked to
# which we can query for optional deps. This basically runs ldd/otoll/objdump
# etc to track deps. We could use a vdb_config binary tools here to improve
# this process

include(GetPrerequisites)

set(_EXCLUDE_SYSTEM_PREREQUISITES 1)
set(_RECURSE_PREREQUISITES 0)
set(_OPENVDB_PREREQUISITE_LIST)

if(NOT OPENVDB_USE_STATIC_LIBS)
get_prerequisites(${OpenVDB_openvdb_LIBRARY}
  _OPENVDB_PREREQUISITE_LIST
  ${_EXCLUDE_SYSTEM_PREREQUISITES}
  ${_RECURSE_PREREQUISITES}
  ""
  "${SYSTEM_LIBRARY_PATHS}"
)
endif()

unset(_EXCLUDE_SYSTEM_PREREQUISITES)
unset(_RECURSE_PREREQUISITES)

# As the way we resolve optional libraries relies on library file names, use
# the configuration options from the main CMakeLists.txt to allow users
# to manually identify the requirements of OpenVDB builds if they know them.

set(OpenVDB_USES_BLOSC ${USE_BLOSC})
set(OpenVDB_USES_LOG4CPLUS ${USE_LOG4CPLUS})
set(OpenVDB_USES_ILM ${USE_EXR})
set(OpenVDB_USES_EXR ${USE_EXR})

# Search for optional dependencies

foreach(PREREQUISITE ${_OPENVDB_PREREQUISITE_LIST})
  set(_HAS_DEP)
  get_filename_component(PREREQUISITE ${PREREQUISITE} NAME)

  string(FIND ${PREREQUISITE} "blosc" _HAS_DEP)
  if(NOT ${_HAS_DEP} EQUAL -1)
    set(OpenVDB_USES_BLOSC ON)
  endif()

  string(FIND ${PREREQUISITE} "log4cplus" _HAS_DEP)
  if(NOT ${_HAS_DEP} EQUAL -1)
    set(OpenVDB_USES_LOG4CPLUS ON)
  endif()

  string(FIND ${PREREQUISITE} "IlmImf" _HAS_DEP)
  if(NOT ${_HAS_DEP} EQUAL -1)
    set(OpenVDB_USES_ILM ON)
  endif()
endforeach()

unset(_OPENVDB_PREREQUISITE_LIST)
unset(_HAS_DEP)

if(OpenVDB_USES_BLOSC)
  find_package(Blosc QUIET)
  if(NOT Blosc_FOUND OR NOT TARGET Blosc::blosc) 
    message(STATUS "find_package could not find Blosc. Using fallback blosc search...")
    find_path(Blosc_INCLUDE_DIR blosc.h)
    find_library(Blosc_LIBRARY NAMES blosc)
    if (Blosc_INCLUDE_DIR AND Blosc_LIBRARY)
      set(Blosc_FOUND TRUE)
      add_library(Blosc::blosc UNKNOWN IMPORTED)
      set_target_properties(Blosc::blosc PROPERTIES 
        IMPORTED_LOCATION "${Blosc_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES ${Blosc_INCLUDE_DIR})
    elseif()
      just_fail("Blosc library can not be found!")
    endif()
  endif()
endif()

if(OpenVDB_USES_LOG4CPLUS)
  find_package(Log4cplus ${_quiet} ${_required})
endif()

if(OpenVDB_USES_ILM)
  find_package(IlmBase ${_quiet} ${_required})
endif()

if(OpenVDB_USES_EXR)
  find_package(OpenEXR ${_quiet} ${_required})
endif()

if(UNIX)
  find_package(Threads ${_quiet} ${_required})
endif()

# Set deps. Note that the order here is important. If we're building against
# Houdini 17.5 we must include OpenEXR and IlmBase deps first to ensure the
# users chosen namespaced headers are correctly prioritized. Otherwise other
# include paths from shared installs (including houdini) may pull in the wrong
# headers

set(_OPENVDB_VISIBLE_DEPENDENCIES
  Boost::iostreams
  Boost::system
  IlmBase::Half
)

set(_OPENVDB_DEFINITIONS)
if(OpenVDB_ABI)
  list(APPEND _OPENVDB_DEFINITIONS "-DOPENVDB_ABI_VERSION_NUMBER=${OpenVDB_ABI}")
endif()

if(OpenVDB_USES_EXR)
  list(APPEND _OPENVDB_VISIBLE_DEPENDENCIES
    IlmBase::IlmThread
    IlmBase::Iex
    IlmBase::Imath
    OpenEXR::IlmImf
  )
  list(APPEND _OPENVDB_DEFINITIONS "-DOPENVDB_TOOLS_RAYTRACER_USE_EXR")
endif()

if(OpenVDB_USES_LOG4CPLUS)
  list(APPEND _OPENVDB_VISIBLE_DEPENDENCIES Log4cplus::log4cplus)
  list(APPEND _OPENVDB_DEFINITIONS "-DOPENVDB_USE_LOG4CPLUS")
endif()

list(APPEND _OPENVDB_VISIBLE_DEPENDENCIES
  TBB::tbb
)
if(UNIX)
  list(APPEND _OPENVDB_VISIBLE_DEPENDENCIES
    Threads::Threads
  )
endif()

set(_OPENVDB_HIDDEN_DEPENDENCIES)

if(OpenVDB_USES_BLOSC)
  if(OPENVDB_USE_STATIC_LIBS)
    list(APPEND _OPENVDB_VISIBLE_DEPENDENCIES $<LINK_ONLY:Blosc::blosc>)
  else()
    list(APPEND _OPENVDB_HIDDEN_DEPENDENCIES Blosc::blosc)
  endif()
endif()

if(OPENVDB_USE_STATIC_LIBS)
  list(APPEND _OPENVDB_VISIBLE_DEPENDENCIES $<LINK_ONLY:ZLIB::ZLIB>)
else()
  list(APPEND _OPENVDB_HIDDEN_DEPENDENCIES ZLIB::ZLIB)
endif()

# ------------------------------------------------------------------------
#  Configure imported target
# ------------------------------------------------------------------------

set(OpenVDB_LIBRARIES
  ${OpenVDB_LIB_COMPONENTS}
)
set(OpenVDB_INCLUDE_DIRS ${OpenVDB_INCLUDE_DIR})

set(OpenVDB_DEFINITIONS)
list(APPEND OpenVDB_DEFINITIONS "${PC_OpenVDB_CFLAGS_OTHER}")
list(APPEND OpenVDB_DEFINITIONS "${_OPENVDB_DEFINITIONS}")
list(REMOVE_DUPLICATES OpenVDB_DEFINITIONS)

set(OpenVDB_LIBRARY_DIRS "")
foreach(LIB ${OpenVDB_LIB_COMPONENTS})
  get_filename_component(_OPENVDB_LIBDIR ${LIB} DIRECTORY)
  list(APPEND OpenVDB_LIBRARY_DIRS ${_OPENVDB_LIBDIR})
endforeach()
list(REMOVE_DUPLICATES OpenVDB_LIBRARY_DIRS)

foreach(COMPONENT ${OpenVDB_FIND_COMPONENTS})
  if(NOT TARGET OpenVDB::${COMPONENT})
    if (${COMPONENT} STREQUAL openvdb)
      include (${CMAKE_CURRENT_LIST_DIR}/CheckAtomic.cmake)
      set(_LINK_LIBS ${_OPENVDB_VISIBLE_DEPENDENCIES} ${CMAKE_REQUIRED_LIBRARIES})
    else ()
      set(_LINK_LIBS _OPENVDB_VISIBLE_DEPENDENCIES)
    endif ()

    add_library(OpenVDB::${COMPONENT} UNKNOWN IMPORTED)
    set_target_properties(OpenVDB::${COMPONENT} PROPERTIES
      INTERFACE_COMPILE_OPTIONS "${OpenVDB_DEFINITIONS}"
      INTERFACE_INCLUDE_DIRECTORIES "${OpenVDB_INCLUDE_DIR}"
      IMPORTED_LINK_DEPENDENT_LIBRARIES "${_OPENVDB_HIDDEN_DEPENDENCIES}" # non visible deps
      INTERFACE_LINK_LIBRARIES "${_LINK_LIBS}" # visible deps (headers)
      INTERFACE_COMPILE_FEATURES cxx_std_11
      IMPORTED_LOCATION "${OpenVDB_${COMPONENT}_LIBRARY}"
   )

   if (_is_multi)
     set_target_properties(OpenVDB::${COMPONENT} PROPERTIES 
       IMPORTED_LOCATION_RELEASE "${OpenVDB_${COMPONENT}_LIBRARY_RELEASE}"
     )

     if (MSVC OR OpenVDB_${COMPONENT}_LIBRARY_DEBUG)
      set_target_properties(OpenVDB::${COMPONENT} PROPERTIES 
        IMPORTED_LOCATION_DEBUG "${OpenVDB_${COMPONENT}_LIBRARY_DEBUG}"
      ) 
     endif ()
   endif ()

   if (OPENVDB_USE_STATIC_LIBS)
    set_target_properties(OpenVDB::${COMPONENT} PROPERTIES
      INTERFACE_COMPILE_DEFINITIONS "OPENVDB_STATICLIB;OPENVDB_OPENEXR_STATICLIB"
    )
   endif()
  endif()
endforeach()

if(OpenVDB_FOUND AND NOT OpenVDB_FIND_QUIETLY)
  message(STATUS "OpenVDB libraries: ${OpenVDB_LIBRARIES}")
endif()

unset(_OPENVDB_DEFINITIONS)
unset(_OPENVDB_VISIBLE_DEPENDENCIES)
unset(_OPENVDB_HIDDEN_DEPENDENCIES)
