# ccache does not parse the -clang: arguments CMake uses for clang-cl's gcc-style
# depfile, so a cache hit writes the object and no depfile, and Ninja then records
# no headers for that object. ccache reproduces /showIncludes output on a hit.
#
# Clang also records the modification time of every input in a precompiled header
# and rejects the header once one differs. ccache caches the compile that creates
# the header, so a tree that regenerates cmake_pch.cxx gets back a header stamped
# with the old file's time, and every compile that uses it fails. The build system
# already rebuilds the header when an input changes. This covers the dependencies,
# such as wxWidgets; PrecompiledHeader.cmake does the same for OrcaSlicer's targets.
foreach (_lang C CXX)
    if (CMAKE_${_lang}_COMPILER_ID STREQUAL "Clang" AND
        CMAKE_${_lang}_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(CMAKE_DEPFILE_FLAGS_${_lang} "/showIncludes")
        set(CMAKE_${_lang}_DEPFILE_FORMAT msvc)
        list(APPEND CMAKE_${_lang}_COMPILE_OPTIONS_CREATE_PCH -Xclang -fno-pch-timestamp)
    endif ()
endforeach ()
