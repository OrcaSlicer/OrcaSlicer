
# This CMake file is written specifically to integrate qhull library with Slic3rPE
# (see https://github.com/prusa3d/PrusaSlicer for more information about the project)
#
# Only original libraries qhullstatic_r and qhullcpp are included.
# They are built as a single statically linked library.
#
# Created by modification of the original qhull CMakeLists.
# Lukas Matena (25.7.2018), lukasmatena@seznam.cz

# see bug report: https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=925540

find_package(Qhull 7.2 QUIET)

add_library(qhull INTERFACE)

if(Qhull_FOUND)

message(STATUS "Using qhull from system.")
if(SLIC3R_STATIC)
    slic3r_remap_configs("Qhull::qhullcpp;Qhull::qhullstatic_r" RelWithDebInfo Release)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        target_link_libraries(qhull INTERFACE Qhull::qhullcpp_d Qhull::qhullstatic_rd)
    else()
        target_link_libraries(qhull INTERFACE Qhull::qhullcpp Qhull::qhullstatic_r)
    endif()
else()
    slic3r_remap_configs("Qhull::qhullcpp;Qhull::qhull_r" RelWithDebInfo Release)
    target_link_libraries(qhull INTERFACE Qhull::qhullcpp Qhull::qhull_r)
endif()

else(Qhull_FOUND)

project(qhull)
cmake_minimum_required(VERSION 3.13)


# Define qhull_VERSION in CMakeLists.txt, Makefile, qhull-exports.def, qhull_p-exports.def, qhull_r-exports.def, qhull-warn.pri
set(qhull_VERSION2 "2015.2 2016/01/18")  # not used, See global.c, global_r.c, rbox.c, rbox_r.c
set(qhull_VERSION     "7.2.0")  # Advance every release

#include(CMakeModules/CheckLFS.cmake)
#option(WITH_LFS "Enable Large File Support" ON)
#check_lfs(WITH_LFS)


message(STATUS "qhull Version: ${qhull_VERSION} (static linking)")


set(libqhull_HEADERS
    # reentrant qhull HEADERS:
        src/libqhull_r/libqhull_r.h
        src/libqhull_r/geom_r.h
        src/libqhull_r/io_r.h
        src/libqhull_r/mem_r.h
        src/libqhull_r/merge_r.h
        src/libqhull_r/poly_r.h
        src/libqhull_r/qhull_ra.h
        src/libqhull_r/qset_r.h
        src/libqhull_r/random_r.h
        src/libqhull_r/stat_r.h
        src/libqhull_r/user_r.h

    # C++ interface to reentrant Qhull HEADERS:
        src/libqhullcpp/Coordinates.h
        src/libqhullcpp/functionObjects.h
        src/libqhullcpp/PointCoordinates.h
        src/libqhullcpp/Qhull.h
        src/libqhullcpp/QhullError.h
        src/libqhullcpp/QhullFacet.h
        src/libqhullcpp/QhullFacetList.h
        src/libqhullcpp/QhullFacetSet.h
        src/libqhullcpp/QhullHyperplane.h
        src/libqhullcpp/QhullIterator.h
        src/libqhullcpp/QhullLinkedList.h
        src/libqhullcpp/QhullPoint.h
        src/libqhullcpp/QhullPoints.h
        src/libqhullcpp/QhullPointSet.h
        src/libqhullcpp/QhullQh.h
        src/libqhullcpp/QhullRidge.h
        src/libqhullcpp/QhullSet.h
        src/libqhullcpp/QhullSets.h
        src/libqhullcpp/QhullStat.h
        src/libqhullcpp/QhullVertex.h
        src/libqhullcpp/QhullVertexSet.h
        src/libqhullcpp/RboxPoints.h
        src/libqhullcpp/RoadError.h
        src/libqhullcpp/RoadLogEvent.h
        src/qhulltest/RoadTest.h
)

set(libqhull_SOURCES
    # reentrant qhull SOURCES:
        src/libqhull_r/global_r.c
        src/libqhull_r/stat_r.c
        src/libqhull_r/geom2_r.c
        src/libqhull_r/poly2_r.c
        src/libqhull_r/merge_r.c
        src/libqhull_r/libqhull_r.c
        src/libqhull_r/geom_r.c
        src/libqhull_r/poly_r.c
        src/libqhull_r/qset_r.c
        src/libqhull_r/mem_r.c
        src/libqhull_r/random_r.c
        src/libqhull_r/usermem_r.c
        src/libqhull_r/io_r.c
        src/libqhull_r/user_r.c
        src/libqhull_r/rboxlib_r.c

    # C++ interface to reentrant Qhull SOURCES:
        src/libqhullcpp/Coordinates.cpp
        src/libqhullcpp/PointCoordinates.cpp
        src/libqhullcpp/Qhull.cpp
        src/libqhullcpp/QhullFacet.cpp
        src/libqhullcpp/QhullFacetList.cpp
        src/libqhullcpp/QhullFacetSet.cpp
        src/libqhullcpp/QhullHyperplane.cpp
        src/libqhullcpp/QhullPoint.cpp
        src/libqhullcpp/QhullPointSet.cpp
        src/libqhullcpp/QhullPoints.cpp
        src/libqhullcpp/QhullQh.cpp
        src/libqhullcpp/QhullRidge.cpp
        src/libqhullcpp/QhullSet.cpp
        src/libqhullcpp/QhullStat.cpp
        src/libqhullcpp/QhullVertex.cpp
        src/libqhullcpp/QhullVertexSet.cpp
        src/libqhullcpp/RboxPoints.cpp
        src/libqhullcpp/RoadError.cpp
        src/libqhullcpp/RoadLogEvent.cpp

    # headers for both (libqhullr and libqhullcpp:
        ${libqhull_HEADERS}
)


##################################################
# combined library (reentrant qhull and qhullcpp) for Slic3r:
set(qhull_STATIC qhullstatic)
add_library(${qhull_STATIC} STATIC ${libqhull_SOURCES})
set_target_properties(${qhull_STATIC} PROPERTIES
    VERSION ${qhull_VERSION})

if(UNIX)
    target_link_libraries(${qhull_STATIC} m)
endif(UNIX)
##################################################

# LIBDIR is defined in the main xs CMake file:
target_include_directories(${qhull_STATIC} SYSTEM BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qhull INTERFACE ${qhull_STATIC})

endif()
