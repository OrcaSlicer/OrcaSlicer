#
# Copyright (c) 2021-2022 Floyd M. Chitalu.
# All rights reserved.
# 
# NOTE: This file is licensed under GPL-3.0-or-later (default). 
# A commercial license can be purchased from Floyd M. Chitalu. 
#  
# License details:
# 
# (A)  GNU General Public License ("GPL"); a copy of which you should have 
#      recieved with this file.
# 	    - see also: <http://www.gnu.org/licenses/>
# (B)  Commercial license.
#      - email: floyd.m.chitalu@gmail.com
# 
# The commercial license options is for users that wish to use MCUT in 
# their products for comercial purposes but do not wish to release their 
# software products under the GPL license. 
# 
# Author(s)     : Floyd M. Chitalu
#
############################################################################################
#
# You can configure MCUT with the following CMake options:
#
#	MCUT_BUILD_AS_SHARED_LIB [default=ON] - Build MCUT as a shared/dynamic library (.so/.dll). -- SET TO OFF
#	MCUT_BUILD_DOCUMENTATION [default=OFF] - Build documentation (explicit dependancy on Doxygen) 
#	MCUT_BUILD_TESTS [default=OFF] - Build the tests (implicit dependancy on GoogleTest)
#	MCUT_BUILD_TUTORIALS [default=OFF] - Build tutorials
#	MCUT_BUILD_WITH_COMPUTE_HELPER_THREADPOOL [default=ON] - Build as configurable multi-threaded library
#
# This script will define the following CMake cache variables:
#
#	MCUT_INCLUDE_DIR - the MCUT include directory
# 	MCUT_LIB_PATH - path to the MCUT library (i.e. .so/.dll or .a/.lib files)

cmake_minimum_required(VERSION 3.13)

project(mcut LANGUAGES CXX C) 

get_directory_property(MCUT_PARENT_DIR PARENT_DIRECTORY)
if(NOT MCUT_PARENT_DIR)
	set(MCUT_TOPLEVEL_PROJECT ON)
else()
	set(MCUT_TOPLEVEL_PROJECT OFF)
endif()

set ( DESCRIPTION "Mesh cutting library." )

if (NOT WIN32 AND NOT CMAKE_BUILD_TYPE)
	message(STATUS "No build type selected, default to Release")
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

set (CMAKE_DEBUG_POSTFIX "d")

#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
#set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list (APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(MCUT_MAJOR 1)
set(MCUT_MINOR 2)
set(MCUT_PATCH 0)

set( MCUT_VERSION "${MCUT_MAJOR}.${MCUT_MINOR}.${MCUT_PATCH}" )

message(STATUS "[MCUT] version: ${MCUT_VERSION}")

set (CMAKE_CXX_STANDARD 11)
set (CMAKE_CXX_STANDARD_REQUIRED True)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

set (project_API_version_string "${MCUT_VERSION}")
set (project_build_version_string "${MCUT_VERSION}")
set (project_namespace_name MCUT)

# Only do these if this is the main project, and not if it is included through add_subdirectory
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)

  # Let's ensure -std=c++xx instead of -std=g++xx
  set(CMAKE_CXX_EXTENSIONS OFF)

  # Let's nicely support folders in IDEs
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)

  # Testing only available if this is the main app
  # Note this needs to be done in the main CMakeLists
  # since it calls enable_testing, which must be in the
  # main CMakeLists.
  include(CTest)

  if(MCUT_BUILD_DOCUMENTATION)
	# Docs only available if this is the main app
	find_package(Doxygen)

	if(Doxygen_FOUND)
		add_subdirectory(docs)
	else()
		message(STATUS "Doxygen not found, not building docs")
	endif()
  endif()
endif()

include(CMakePrintHelpers) 

#
# User options
#
option(MCUT_BUILD_DOCUMENTATION "Configure to build docs with Doxygen" OFF) # OFF by default
if (MCUT_TOPLEVEL_PROJECT AND NOT MCUT_BUILD_TESTS)
	option(MCUT_BUILD_TESTS "Configure to build tests" ON)
endif()
option(MCUT_BUILD_AS_SHARED_LIB "Configure to build MCUT as a shared/dynamic library" OFF)
option(MCUT_BUILD_WITH_COMPUTE_HELPER_THREADPOOL "Configure to build MCUT engine with a shared (amongst contexts) thread-pool" ON)
if (MCUT_TOPLEVEL_PROJECT AND NOT MCUT_BUILD_TUTORIALS) 
	option(MCUT_BUILD_TUTORIALS "Configure to build MCUT tutorials" ON)
endif()
#
# machine-precision-numbers library targets
#
set (target_name mcut)

#
# MCUT compilation variables/settings
#
set (MCUT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include CACHE STRING "The MCUT include directory")

message(STATUS "[MCUT] MCUT_INCLUDE_DIR=${MCUT_INCLUDE_DIR}")

list(APPEND include_dirs  ${MCUT_INCLUDE_DIR})
list(APPEND compilation_flags "")
if(MCUT_BUILD_AS_SHARED_LIB)
	list(APPEND preprocessor_defs -DMCUT_SHARED_LIB=1)
endif()

find_package(Threads REQUIRED)

list(APPEND extra_libs Threads::Threads) 

if (MCUT_BUILD_WITH_COMPUTE_HELPER_THREADPOOL)
	list(APPEND preprocessor_defs -DMCUT_WITH_COMPUTE_HELPER_THREADPOOL=1)
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	list(APPEND compilation_flags -Wall -Wextra)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Intel")
	list(APPEND compilation_flags w3 -diag-disable:remark)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	list(APPEND compilation_flags /W4 /wd26812 /wd4131 /wd4704 /wd4245 /wd4297 /wd4505 /wd4701 /bigobj)
	list(APPEND preprocessor_defs -D_CRT_SECURE_NO_WARNINGS)
endif()

message(STATUS "[MCUT] compilation_flags=${compilation_flags}")
message(STATUS "[MCUT] preprocessor_defs=${preprocessor_defs}")
message(STATUS "[MCUT] extra_libs=${extra_libs}")

set (	project_source_files  
		${CMAKE_CURRENT_SOURCE_DIR}/source/mcut.cpp 
		${CMAKE_CURRENT_SOURCE_DIR}/source/kernel.cpp 
		${CMAKE_CURRENT_SOURCE_DIR}/source/hmesh.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/source/math.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/source/bvh.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/source/shewchuk.c
		${CMAKE_CURRENT_SOURCE_DIR}/source/frontend.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/source/preproc.cpp)

#
# Create MCUT target(s)
#

# This function invokes commands which create a library target (static, shared etc.)
function(create_library_target LIBRARY_TYPE)
    message(STATUS "[MCUT] create target: name=${target_name} type=${LIBRARY_TYPE}")
	
	add_library(${target_name} ${LIBRARY_TYPE} ${project_source_files})

        target_include_directories(${target_name} SYSTEM PRIVATE ${include_dirs})
	target_link_libraries(${target_name} PRIVATE ${extra_libs})
	target_compile_options(${target_name} PRIVATE ${compilation_flags})
	target_compile_definitions(${target_name} PRIVATE ${preprocessor_defs} )
	
	if (MCUT_BUILD_AS_SHARED_LIB AND WIN32)
		# add macro to export .dll symbols
		target_compile_definitions(${target_name} PRIVATE -DMCUT_EXPORT_SHARED_LIB_SYMBOLS=1)
	endif()

	set_property(TARGET ${target_name} PROPERTY VERSION ${project_build_version_string})
	set_property(TARGET ${target_name} PROPERTY SOVERSION ${project_API_version_string})

	get_target_property(target_type ${target_name} TYPE)

	if ("${target_type}" STREQUAL "SHARED")
		set_property(TARGET ${target_name} PROPERTY POSITION_INDEPENDENT_CODE ON)
		if(MSVC)
			set_property(TARGET ${target_name} PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS ON)
		endif()
	endif()
endfunction()

#
# create target
#
if(MCUT_BUILD_AS_SHARED_LIB)
	create_library_target(SHARED) 
else()
	create_library_target(STATIC)
endif()

set (MCUT_LIB_PATH $<TARGET_FILE:mcut>  CACHE STRING "Path to the compiled MCUT library file")
message(STATUS "[MCUT] MCUT_LIB_PATH=${MCUT_LIB_PATH}")

#
# tests, tutorials etc. are dependant on third-party libs (file loaders etc.)
# We enable the code that is used to download those projects here:
#
if(${MCUT_BUILD_TESTS} OR ${MCUT_BUILD_TUTORIALS})

	# FetchContent added in CMake 3.11, downloads during the configure step
	include(FetchContent)
	# FetchContent_MakeAvailable was not added until CMake 3.14; use our shim
	if(${CMAKE_VERSION} VERSION_LESS 3.14)
		include(cmake/add_FetchContent_MakeAvailable.cmake)
	endif()

	FetchContent_Populate(
		libigl
		GIT_REPOSITORY 			https://github.com/libigl/libigl.git
		GIT_TAG       	v2.3.0
		GIT_PROGRESS TRUE
	)

	#set(libigl_include_dir ${CMAKE_BINARY_DIR}/libigl-src/include)
	set(libigl_include_dir ${libigl_SOURCE_DIR}/include)

	set(LIBIGL_EIGEN_VERSION 3.3.7 CACHE STRING "Default version of Eigen used by libigl.")

	# used by tests & tutorials
	#download_project(PROJ                eigen
	#				 GIT_REPOSITORY      https://gitlab.com/libeigen/eigen.git
	#				 GIT_TAG             ${LIBIGL_EIGEN_VERSION}
	#				 ${UPDATE_DISCONNECTED_IF_AVAILABLE}
	#)
	FetchContent_Declare(
		eigen
		GIT_REPOSITORY 			https://gitlab.com/libeigen/eigen.git
		GIT_TAG       	${LIBIGL_EIGEN_VERSION}
		GIT_SHALLOW TRUE
  		GIT_PROGRESS TRUE
	)

	set(EIGEN_BUILD_DOC OFF)
	# note: To disable eigen tests,
	# you should put this code in a add_subdirectory to avoid to change
	# BUILD_TESTING for your own project too since variables are directory
	# scoped
	set(BUILD_TESTING OFF)
	set(EIGEN_BUILD_PKGCONFIG OFF)
	set( OFF)
	FetchContent_MakeAvailable(eigen)

	#set(eigen_include_dir ${CMAKE_BINARY_DIR}/eigen-src)
	set(eigen_include_dir ${eigen_SOURCE_DIR})
endif()

#
# tests
#
if(MCUT_BUILD_TESTS)
	add_subdirectory(tests)
endif()

#
# tutorials
#
if(MCUT_BUILD_TUTORIALS)
	add_subdirectory(tutorials)
endif()

#
# documentation
#
if(MCUT_BUILD_DOCUMENTATION)
	add_subdirectory(docs)
endif()

########################################################
###                   PACKAGING                      ###
### This is a quite INCOMPLETE set of variables that ###
### should be set for the various generators.        ###
### Consult the CPack documentations for a full set. ###
########################################################

# https://gitlab.kitware.com/cmake/community/-/wikis/doc/cpack/Component-Install-With-CPack
# https://stackoverflow.com/questions/6003374/what-is-cmake-equivalent-of-configure-prefix-dir-make-all-install

# TODO: package documentation files

# if(MCUT_BUILD_AS_SHARED_LIB)
# 	#
# 	# dynamic libs
# 	#

# 	install(TARGETS ${mpn_shared_lib_name}
# 			LIBRARY
# 			DESTINATION lib/shared
# 			COMPONENT dynamic_libraries)
# else()
# 	#
# 	# static libs
# 	#
	
# 	install(TARGETS ${mpn_static_lib_name}
# 			ARCHIVE
# 			DESTINATION lib/static
# 			COMPONENT static_libraries)
# endif()

#
# headers
#
# install(FILES ${MCUT_INCLUDE_DIR}/mcut/mcut.h ${MCUT_INCLUDE_DIR}/mcut/platform.h
#   		DESTINATION include/mcut
# 	  	COMPONENT headers)
	  
# install(FILES 
# 		${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt
# 		${CMAKE_CURRENT_SOURCE_DIR}/README.md
# 		DESTINATION ./
# 		COMPONENT text_files)

#
# notify CPack of the names of all of the components in the project
#
# set(CPACK_COMPONENTS_ALL static_libraries dynamic_libraries headers text_files) # applications

# set(CPACK_COMPONENT_APPLICATIONS_DISPLAY_NAME "MCUT Application")
# set(CPACK_COMPONENT_STATIC_LIBRARIES_DISPLAY_NAME "Static Libraries")
# set(CPACK_COMPONENT_DYNAMIC_LIBRARIES_DISPLAY_NAME "Dynamics Libraries")
# set(CPACK_COMPONENT_HEADERS_DISPLAY_NAME "C++ Headers")

# set(CPACK_COMPONENT_APPLICATIONS_DESCRIPTION
#   "A simple application using MCUT")
# set(CPACK_COMPONENT_STATIC_LIBRARIES_DESCRIPTION
#   "Static libraries used to build programs with MCUT")
#   set(CPACK_COMPONENT_DYNAMIC_LIBRARIES_DESCRIPTION
#   "Dynamic libraries used to build programs with MCUT")
# set(CPACK_COMPONENT_HEADERS_DESCRIPTION
#   "C/C++ header files for use with MCUT")

# #  
# # component dependencies
# #
# set(CPACK_COMPONENT_HEADERS_DEPENDS static_libraries dynamic_libraries)

# set(CPACK_COMPONENT_APPLICATIONS_GROUP "Runtime")
# set(CPACK_COMPONENT_STATIC_LIBRARIES_GROUP "Development")
# set(CPACK_COMPONENT_DYNAMIC_LIBRARIES_GROUP "Development")
# set(CPACK_COMPONENT_HEADERS_GROUP "Development")

# set(CPACK_COMPONENT_GROUP_DEVELOPMENT_DESCRIPTION
#   "All of the tools you'll ever need to develop software")

# set (CPACK_PACKAGE_NAME "MCUT")
# set (CPACK_PACKAGE_VENDOR "Floyd M. Chitalu")
# set (CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# set (CPACK_PACKAGE_VERSION_MAJOR "${MCUT_MAJOR}")
# set (CPACK_PACKAGE_VERSION_MINOR "${MCUT_MINOR}")
# set (CPACK_PACKAGE_VERSION_PATCH "${MCUT_PATCH}")
# #set (CPACK_PACKAGE_DESCRIPTION "MCUT (pronounced ‘emcut’) is a tool for cutting meshes.")
# #set (CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/DESCRIPTION.txt)
# set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "MCUT is a library for cutting meshes to perform tasks like boolean operations and more.")
# set (CPACK_PACKAGE_HOMEPAGE_URL "https://cutdigital.github.io/mcut.site/")
# set (CPACK_PACKAGE_INSTALL_DIRECTORY ${CPACK_PACKAGE_NAME})
# # set (CPACK_PACKAGE_ICON ) 
# set (CPACK_PACKAGE_CHECKSUM SHA256)
# #set (CPACK_PROJECT_CONFIG_FILE ) 
# set (CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt) # must also include in install command
# set (CPACK_RESOURCE_FILE_README ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
# #set (CPACK_RESOURCE_FILE_WELCOME ${CMAKE_CURRENT_SOURCE_DIR}/WELCOME.txt)

if (WIN32)
    if (USE_WIX_TOOLSET)
        set(CPACK_GENERATOR "WIX") # this need WiX Tooset installed and a path to candle.exe
    else ()
        set(CPACK_GENERATOR "NSIS") # this needs NSIS installed, and available
    endif ()
elseif ( ${CMAKE_SYSTEM_NAME} MATCHES "Darwin" )
    set(CPACK_GENERATOR "PackageMake")
else ()
    set(CPACK_GENERATOR "TGZ")
endif ()

#set (CPACK_OUTPUT_CONFIG_FILE ) # Defaults to CPackConfig.cmake.
#set (CPACK_PACKAGE_EXECUTABLES ) 
# set (CPACK_STRIP_FILES TRUE)
# set (CPACK_VERBATIM_VARIABLES )
# set (CPACK_SOURCE_PACKAGE_FILE_NAME )
# set (CPACK_SOURCE_STRIP_FILES )
# set (CPACK_SOURCE_GENERATOR )
# set (CPACK_SOURCE_OUTPUT_CONFIG_FILE )
# set (CPACK_SOURCE_IGNORE_FILES )
# set (CPACK_CMAKE_GENERATOR )
# set (CPACK_INSTALL_CMAKE_PROJECTS ) 
# set (CPACK_INSTALL_CMAKE_PROJECTS )
# set (CPACK_SYSTEM_NAME )
# set (CPACK_PACKAGE_VERSION )
# set (CPACK_TOPLEVEL_TAG )
# set (CPACK_INSTALL_COMMANDS )
# set (CPACK_INSTALLED_DIRECTORIES )
# set ( )


# include(CPack)

# eof
