function(check_portability_forbidden_includes portability_dir)
    if (NOT EXISTS "${portability_dir}")
        message(STATUS "Portability include check skipped: ${portability_dir} does not exist")
        return()
    endif()

    file(GLOB_RECURSE portability_sources
        "${portability_dir}/*.h"
        "${portability_dir}/*.hh"
        "${portability_dir}/*.hpp"
        "${portability_dir}/*.hxx"
        "${portability_dir}/*.c"
        "${portability_dir}/*.cc"
        "${portability_dir}/*.cpp"
        "${portability_dir}/*.cxx"
        "${portability_dir}/*.ipp"
        "${portability_dir}/*.inl")

    if (NOT portability_sources)
        message(STATUS "Portability include check skipped: no source files found in ${portability_dir}")
        return()
    endif()

    set(forbidden_patterns
        "[ \\t]*#[ \\t]*include[ \\t]*[<\"][ \\t]*wx/"
        "[ \\t]*#[ \\t]*include[ \\t]*[<\"][ \\t]*GL/"
        "[ \\t]*#[ \\t]*include[ \\t]*[<\"][ \\t]*OpenGL/")

    set(violations)

    foreach(source_file IN LISTS portability_sources)
        foreach(pattern IN LISTS forbidden_patterns)
            file(STRINGS "${source_file}" matched_lines REGEX "${pattern}")
            if (matched_lines)
                foreach(line IN LISTS matched_lines)
                    list(APPEND violations "${source_file}: ${line}")
                endforeach()
            endif()
        endforeach()
    endforeach()

    if (violations)
        string(JOIN "\n  " violation_output ${violations})
        message(FATAL_ERROR
            "Forbidden includes were found under src/portability.\n"
            "The portability layer must not include wxWidgets or desktop OpenGL headers.\n"
            "Violations:\n"
            "  ${violation_output}")
    endif()
endfunction()
