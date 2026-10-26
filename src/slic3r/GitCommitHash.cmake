# Writes GIT_COMMIT_HASH and GIT_COMMIT_SUFFIX into a generated header.
# GIT_COMMIT_SUFFIX is "-dirty" for a build with uncommitted changes, and empty
# otherwise.
#
# A custom target runs this at the start of every build, which picks up a new
# commit without a reconfigure. The header is rewritten only when the value
# changes.
#
# Inputs: SOURCE_DIR, OUT_FILE.

find_package(Git QUIET)

set(HASH "")
set(SUFFIX "")

if (DEFINED ENV{git_commit_hash} AND NOT "$ENV{git_commit_hash}" STREQUAL "")
    if (GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short "$ENV{git_commit_hash}"
            WORKING_DIRECTORY ${SOURCE_DIR} OUTPUT_VARIABLE HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
    else ()
        # No .git directory (e.g. Flatpak sandbox) - truncate directly
        string(SUBSTRING "$ENV{git_commit_hash}" 0 7 HASH)
    endif ()
elseif (GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
        WORKING_DIRECTORY ${SOURCE_DIR} OUTPUT_VARIABLE HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD
        WORKING_DIRECTORY ${SOURCE_DIR} RESULT_VARIABLE DIRTY ERROR_QUIET)
    if (DIRTY EQUAL 1)
        set(SUFFIX "-dirty")
    endif ()
endif ()

if (NOT HASH)
    set(HASH "0000000") # uninitialized
endif ()

message(STATUS "Build commit: ${HASH}${SUFFIX}")

string(CONCAT CONTENT
    "#pragma once\n"
    "#define GIT_COMMIT_HASH \"${HASH}\"\n"
    "#define GIT_COMMIT_SUFFIX \"${SUFFIX}\"\n")

set(OLD "")
if (EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}" OLD)
endif ()
if (NOT OLD STREQUAL CONTENT)
    file(WRITE "${OUT_FILE}" "${CONTENT}")
endif ()
