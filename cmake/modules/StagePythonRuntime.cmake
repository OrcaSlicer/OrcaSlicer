# cmake -DSOURCE=<dir> -DDESTINATION=<dir> -DSTAMP=<file> -P StagePythonRuntime.cmake
# Copies only when SOURCE's files or their times changed since the copy STAMP records.
foreach (_var SOURCE DESTINATION STAMP)
    if (NOT DEFINED ${_var})
        message(FATAL_ERROR "StagePythonRuntime.cmake: ${_var} is not set")
    endif ()
endforeach ()

file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${SOURCE}" "${SOURCE}/*")
list(SORT _files)
set(_signature "${SOURCE}\n")
foreach (_file IN LISTS _files)
    file(TIMESTAMP "${SOURCE}/${_file}" _time "%Y%m%d%H%M%S" UTC)
    string(APPEND _signature "${_file} ${_time}\n")
endforeach ()
string(SHA256 _signature "${_signature}")

if (IS_DIRECTORY "${DESTINATION}" AND EXISTS "${STAMP}")
    file(READ "${STAMP}" _copied)
    if (_copied STREQUAL _signature)
        return()
    endif ()
endif ()

file(REMOVE "${STAMP}")
file(REMOVE_RECURSE "${DESTINATION}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SOURCE}" "${DESTINATION}"
    RESULT_VARIABLE _result)
if (NOT _result EQUAL 0)
    message(FATAL_ERROR "Copying ${SOURCE} to ${DESTINATION} failed")
endif ()
file(WRITE "${STAMP}" "${_signature}")
