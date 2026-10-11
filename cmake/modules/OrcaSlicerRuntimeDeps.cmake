include_guard(GLOBAL)

# Copies runtime shared libraries next to each test executable, and orca_bench. Handles both
# single-config generators (CMAKE_BUILD_TYPE set) and multi-config generators
# (Ninja Multi-Config, Visual Studio) where CMAKE_BUILD_TYPE is empty and DLLs
# must land in every per-config output directory. On Windows the loader finds
# DLLs in the executable's directory; the Linux branch below does the same for
# the deps-built FFmpeg libraries and adds an $ORIGIN rpath, since the ELF
# loader does not search the executable's directory and the CI unit-test runner
# only receives the tests artifact (no deps install).
function(orcaslicer_copy_test_dlls)
    if (WIN32)
        set(_configs ${CMAKE_CONFIGURATION_TYPES})
        if (NOT _configs)
            set(_configs "${CMAKE_BUILD_TYPE}")
        endif()
        foreach(_cfg IN LISTS _configs)
            if (_cfg STREQUAL "Debug")
                orcaslicer_copy_dlls(COPY_DLLS "Debug" "d" _unused_dlls)
            else()
                orcaslicer_copy_dlls(COPY_DLLS "${_cfg}" "" _unused_dlls)
            endif()
        endforeach()
    elseif (UNIX AND NOT APPLE)
        # Only test executables that link libslic3r_gui pull in the FFmpeg
        # shared libraries (src/slic3r/CMakeLists.txt links PkgConfig::LIBAV
        # into it). Copy them next to the executable and give it an $ORIGIN
        # rpath so the loader finds them when the tests run on the CI unit-test
        # runner, which only receives this build/tests tree.
        #
        # Optional first arg: the target to patch, for directories that define
        # more than one test executable. Defaults to ${_TEST_NAME}_tests so
        # existing single-executable callers don't need to pass it.
        if (ARGC GREATER 0)
            set(_target ${ARGV0})
        else()
            set(_target ${_TEST_NAME}_tests)
        endif()

        get_target_property(_linked_libs ${_target} LINK_LIBRARIES)
        if (NOT "libslic3r_gui" IN_LIST _linked_libs)
            return()
        endif()

        set_property(TARGET ${_target} PROPERTY BUILD_RPATH "$ORIGIN")
        set(_configs ${CMAKE_CONFIGURATION_TYPES})
        if (NOT _configs)
            set(_configs "${CMAKE_BUILD_TYPE}")
        endif()
        foreach(_cfg IN LISTS _configs)
            orcaslicer_copy_sos(${_target} "${_cfg}" "" _unused_sos)
        endforeach()
    endif()
endfunction()
