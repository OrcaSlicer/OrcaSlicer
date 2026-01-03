get_patch_dir_flag(NLopt)

orcaslicer_add_cmake_project(NLopt
  URL "https://github.com/stevengj/nlopt/archive/v2.5.0.tar.gz"
  URL_HASH SHA256=c6dd7a5701fff8ad5ebb45a3dc8e757e61d52658de3918e38bab233e7fd3b4ae
  PATCH_COMMAND ${PATCH_CMD} ${NLopt_dir_flag} ${CMAKE_CURRENT_LIST_DIR}/clangcl.patch
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
