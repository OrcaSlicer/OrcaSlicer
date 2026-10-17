# Header-only as far as CMake is concerned. The explicit instantiations in
# src/libnest2d.cpp need libslic3r's geometry, and libslic3r uses them, so the
# file is compiled as part of libslic3r rather than a library of its own: two
# libraries that link each other are allowed only when both are STATIC.
add_library(libnest2d INTERFACE)

target_include_directories(libnest2d SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(libnest2d INTERFACE NLopt::nlopt TBB::tbb Boost::boost)
target_compile_definitions(libnest2d INTERFACE LIBNEST2D_THREADING_tbb LIBNEST2D_STATIC LIBNEST2D_OPTIMIZER_nlopt LIBNEST2D_GEOMETRIES_libslic3r)
