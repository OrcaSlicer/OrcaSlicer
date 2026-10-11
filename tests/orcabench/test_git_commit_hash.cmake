# Runs GitCommitHash.cmake (SCRIPT) against a scratch repository in WORK_DIR and fails unless an
# untracked source among GLOBBED_DIRS makes the build dirty and an untracked file elsewhere, or in a
# directory below one of them, does not.
find_package(Git QUIET)
if (NOT GIT_FOUND)
    message("SKIP: git is not installed")
    return()
endif ()
# A commit named by the environment takes the script's branch that never checks the working copy.
unset(ENV{git_commit_hash})

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/kinds/nested")
execute_process(COMMAND "${GIT_EXECUTABLE}" init -q WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "git init failed in ${WORK_DIR}")
endif ()

# The suffix the script writes for the scratch repository as it stands.
set(header_file "${WORK_DIR}.h")
function(suffix out)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${WORK_DIR}" "-DOUT_FILE=${header_file}" -DGLOBBED_DIRS=kinds
                            -P "${SCRIPT}"
                    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "${SCRIPT} failed in ${WORK_DIR}")
    endif ()
    file(READ "${header_file}" header)
    string(REGEX MATCH "GIT_COMMIT_SUFFIX \"([^\"]*)\"" matched "${header}")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

file(WRITE "${WORK_DIR}/notes.txt" "")
file(WRITE "${WORK_DIR}/kinds/nested/helper.cpp" "")
suffix(elsewhere)
file(WRITE "${WORK_DIR}/kinds/new_kind.cpp" "")
suffix(globbed)
if (NOT elsewhere STREQUAL "" OR NOT globbed STREQUAL "-dirty")
    message(FATAL_ERROR "an untracked file elsewhere gave the suffix \"${elsewhere}\" and an untracked "
                        "source among the globbed ones gave \"${globbed}\", where \"\" and \"-dirty\" were expected")
endif ()
