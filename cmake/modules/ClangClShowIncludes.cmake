# ccache does not parse the -clang: arguments CMake uses for clang-cl's gcc-style
# depfile, so a cache hit writes the object and no depfile, and Ninja then records
# no headers for that object. ccache reproduces /showIncludes output on a hit.
foreach (_lang C CXX)
    if (CMAKE_${_lang}_COMPILER_ID STREQUAL "Clang" AND
        CMAKE_${_lang}_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(CMAKE_DEPFILE_FLAGS_${_lang} "/showIncludes")
        set(CMAKE_${_lang}_DEPFILE_FORMAT msvc)
    endif ()
endforeach ()
