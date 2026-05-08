cmake_minimum_required(VERSION 3.13)
project(deps_src)

# Include all dependency subdirectories
set(USE_SYS_LIBS ON)  # Flag to indicate whether to use system libraries

# Header-only libraries (INTERFACE)
add_subdirectory(agg)
add_subdirectory(ankerl)
add_subdirectory(earcut)

find_package(FastFloat QUIET)
if (USE_SYS_LIBS AND FastFloat_FOUND)
    message(STATUS "Using system FastFloat library")
else()
    add_subdirectory(fast_float)
endif()

find_package(NanoSVG QUIET)
if (USE_SYS_LIBS AND NanoSVG_FOUND)
    message(STATUS "Using system NanoSVG library")
else()
    add_subdirectory(nanosvg)
endif()

find_package(nlohmann_json QUIET)
if (USE_SYS_LIBS AND nlohmann_json_FOUND)
    message(STATUS "Using system nlohmann_json library")
else()
    add_subdirectory(nlohmann)
endif()

add_subdirectory(stb_dxt)     # Header-only STB DXT compression library

# Static libraries
add_subdirectory(Shiny)
add_subdirectory(admesh)
add_subdirectory(clipper)

find_package(Clipper2 QUIET)
if (USE_SYS_LIBS AND Clipper2_FOUND)
    message(STATUS "Using system Clipper2 library")
else()
    add_subdirectory(clipper2)
endif()

find_package(expat QUIET)
if (USE_SYS_LIBS AND expat_FOUND)
    message(STATUS "Using system expat library")
else()
    add_subdirectory(expat)
endif()

add_subdirectory(glu-libtess)

find_package(hodapi QUIET)
if (USE_SYS_LIBS AND hidapi_FOUND)
    message(STATUS "Using system hidapi library")
else()
    add_subdirectory(hidapi)
endif()

add_subdirectory(hints)        # Hints library with utility executable
add_subdirectory(imgui)
add_subdirectory(imguizmo)
add_subdirectory(libigl)
add_subdirectory(libnest2d)
add_subdirectory(mcut)

find_package(md4c QUIET)
if (USE_SYS_LIBS AND md4c_FOUND)
    message(STATUS "Using system md4c library")
else()
    add_subdirectory(md4c)
endif()

add_subdirectory(miniz)
add_subdirectory(minilzo)
add_subdirectory(qhull)
add_subdirectory(qoi)
add_subdirectory(semver)      # Semver static library

# Eigen header-only library
add_subdirectory(eigen)
