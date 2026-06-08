
option(SLIC3R_ENC_CHECK "Verify encoding of source files" 1)

if (IS_CROSS_COMPILE)
    # Force disable due to cross compilation. This fact is already printed on cli for users
    set(SLIC3R_ENC_CHECK OFF CACHE BOOL "" FORCE)
endif ()

if (SLIC3R_ENC_CHECK)
    add_executable(encoding-check encoding-check.cpp)

    # A global no-op target which depends on all encodings checks,
    # and on which in turn all checked targets depend.
    # This is done to make encoding checks the first thing to be
    # performed before actually compiling any sources of the checked targets
    # to make the check fail as early as possible.
    add_custom_target(global-encoding-check
        ALL
        DEPENDS encoding-check
    )
endif()

# Function that adds source file encoding check to a target
# using the above encoding-check binary

function(encoding_check TARGET)
    if (SLIC3R_ENC_CHECK)
        # Obtain target source files
        get_target_property(T_SOURCES ${TARGET} SOURCES)

        set(STAMP_FILE  "${CMAKE_CURRENT_BINARY_DIR}/encoding-check-${TARGET}.stamp")
        set(RSP_FILE    "${CMAKE_CURRENT_BINARY_DIR}/encoding-check-${TARGET}.rsp")

        # Resolve absolute paths
        set(ABS_SOURCES "")
        foreach(file ${T_SOURCES})
            if(IS_ABSOLUTE "${file}")
                list(APPEND ABS_SOURCES "${file}")
            else()
                list(APPEND ABS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
            endif()
        endforeach()

        # Write response file at configure time
        string(REPLACE ";" "\n" RSP_CONTENT "${ABS_SOURCES}")
        file(WRITE "${RSP_FILE}" "${RSP_CONTENT}")

        # Single process call via response file — no command line length limit
        add_custom_command(
            OUTPUT  "${STAMP_FILE}"
            DEPENDS encoding-check ${ABS_SOURCES}
            COMMENT "Checking source files encodings for target ${TARGET}"
            COMMAND $<TARGET_FILE:encoding-check> ${TARGET} "@${RSP_FILE}"
            COMMAND ${CMAKE_COMMAND} -E touch "${STAMP_FILE}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )

        add_custom_target(encoding-check-${TARGET} DEPENDS "${STAMP_FILE}")
        add_dependencies(global-encoding-check encoding-check-${TARGET})
        add_dependencies(${TARGET} global-encoding-check)
    endif()
endfunction()
