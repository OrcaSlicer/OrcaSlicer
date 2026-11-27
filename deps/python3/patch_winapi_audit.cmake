# Applied as PATCH_COMMAND for CPython on Windows, from the extracted source directory.
#
# CPython 3.12.13 raises the _winapi.CreateProcess audit event with PySys_Audit format "uuu",
# although command_line is a Python object: audit hooks receive garbage instead of the command
# line. PluginAuditManager turns that argument into a process grant's target, so the prompt
# named the wrong executable and the grant it saved could not cover a real call. The patch
# changes the format to "uOu", as upstream does (python/cpython#159237), until the pinned
# CPython release carries the fix.
#
# Applying is idempotent: ExternalProject re-runs the patch step when the patch stamp is missing,
# which can happen on a source tree that already carries the patch. A reverse check recognises
# that state instead of failing the build on an already patched file.
# When the pinned CPython release carries the fix, drop this script, its patch and PATCH_COMMAND.

set(_patch "${CMAKE_CURRENT_LIST_DIR}/0001-winapi-createprocess-audit-format.patch")

if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "GIT_EXECUTABLE is required")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --ignore-space-change "${_patch}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)
if(_already_applied EQUAL 0)
    message(STATUS "CPython: _winapi.CreateProcess audit format is already patched")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --verbose --ignore-space-change --whitespace=fix "${_patch}"
    RESULT_VARIABLE _apply_result
)
if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR "CPython: failed to apply ${_patch} (exit ${_apply_result})")
endif()
