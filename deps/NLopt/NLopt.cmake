orcaslicer_add_cmake_project(NLopt
  URL "https://github.com/stevengj/nlopt/archive/v2.11.0.tar.gz"
  URL_HASH SHA256=dba382b19849922f30073318e04d79bb5acd453b87b9e02f4b9ffc67009e979c
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
