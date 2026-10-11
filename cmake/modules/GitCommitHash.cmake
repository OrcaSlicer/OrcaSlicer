# Writes GIT_COMMIT_HASH and GIT_COMMIT_SUFFIX into a generated header.
# GIT_COMMIT_SUFFIX is "-dirty" for a build with uncommitted changes to tracked
# files or an untracked source in GLOBBED_DIRS, and empty otherwise.
#
# A custom target runs this at the start of every build, which picks up a new
# commit without a reconfigure. The header is rewritten only when the value
# changes.
#
# Inputs: SOURCE_DIR, OUT_FILE, and optionally GLOBBED_DIRS, the directories,
# relative to SOURCE_DIR, whose .cpp files a build picks up by glob.

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
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${SOURCE_DIR} OUTPUT_VARIABLE HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD
        WORKING_DIRECTORY ${SOURCE_DIR} RESULT_VARIABLE DIRTY ERROR_QUIET)
    if (DIRTY EQUAL 1)
        set(SUFFIX "-dirty")
    elseif (GLOBBED_DIRS)
        # :(glob) keeps * from matching /, as file(GLOB) does.
        list(TRANSFORM GLOBBED_DIRS PREPEND ":(glob)" OUTPUT_VARIABLE GLOBBED_SOURCES)
        list(TRANSFORM GLOBBED_SOURCES APPEND "/*.cpp")
        execute_process(COMMAND ${GIT_EXECUTABLE} ls-files --others --exclude-standard
                                -- ${GLOBBED_SOURCES}
            WORKING_DIRECTORY ${SOURCE_DIR} OUTPUT_VARIABLE UNTRACKED ERROR_QUIET)
        if (NOT UNTRACKED STREQUAL "")
            set(SUFFIX "-dirty")
        endif ()
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
