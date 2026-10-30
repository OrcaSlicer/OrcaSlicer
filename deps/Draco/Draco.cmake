set(_options "")
if (MSVC AND DEP_DEBUG)
    set(_options "FORWARD_CONFIG")
endif ()

orcaslicer_add_cmake_project(Draco
    ${_options}
    URL https://github.com/google/draco/archive/refs/tags/1.5.9.zip
    URL_HASH SHA256=ee89f62f688088af64314bdd81be9389b8b8b8932c12f2622e2804d248c56378
    CMAKE_ARGS
        # The encoder and decoder tools duplicate draco.lib; see deps-windows.cmake.
        "${DEP_LLD_FORCE_MULTIPLE}"
)
