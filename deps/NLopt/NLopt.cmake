orcaslicer_add_cmake_project(NLopt
  URL "https://github.com/stevengj/nlopt/archive/v2.11.0.tar.gz"
  URL_HASH SHA256=53e552d83e9294d67db37f0f4a23f15933a9ef698485301a18b98b40004cf0de
  CMAKE_ARGS
    -DNLOPT_PYTHON:BOOL=OFF
    -DNLOPT_OCTAVE:BOOL=OFF
    -DNLOPT_MATLAB:BOOL=OFF
    -DNLOPT_GUILE:BOOL=OFF
    -DNLOPT_SWIG:BOOL=OFF
    -DNLOPT_TESTS:BOOL=OFF
)

if (MSVC)
    add_debug_dep(dep_NLopt)
endif ()
