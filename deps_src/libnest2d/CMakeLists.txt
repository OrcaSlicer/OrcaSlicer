# libnest2d is a header-only template library used by libslic3r's arrangement
# code. Previously it shipped a 26-line .cpp file with explicit template
# instantiations against libslic3r's geometry types — but that introduced a
# libnest2d → libslic3r → libnest2d link cycle, which CMake forbids once
# libslic3r becomes SHARED (Phase 0.3 engine extraction). Converting the
# library to INTERFACE drops the .cpp and lets consumers instantiate the
# templates implicitly via vague linkage; the linker dedupes via comdat.

add_library(libnest2d INTERFACE)

target_include_directories(libnest2d SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(libnest2d INTERFACE NLopt::nlopt TBB::tbb Boost::boost)
target_compile_definitions(libnest2d INTERFACE LIBNEST2D_THREADING_tbb LIBNEST2D_STATIC LIBNEST2D_OPTIMIZER_nlopt LIBNEST2D_GEOMETRIES_libslic3r)
