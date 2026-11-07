# Moves the hms_*.json files of a staged resources/hms into one hms.zip; the repository keeps the plain JSON.
# Usage: cmake -DHMS_DIR=<staged resources/hms> -P PackHmsResources.cmake
# The JSON files are deleted, so HMS_DIR must never be the source tree.

cmake_minimum_required(VERSION 3.13)

if(NOT HMS_DIR)
    message(FATAL_ERROR "PackHmsResources: HMS_DIR is not set")
endif()
# file(GLOB ... RELATIVE) finds nothing when the base is relative, as with the AppImage's APPDIR="package".
# In script mode a relative path resolves against the current directory.
get_filename_component(HMS_DIR "${HMS_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${HMS_DIR}")
    message(FATAL_ERROR "PackHmsResources: HMS_DIR '${HMS_DIR}' is not a directory")
endif()

# Dev builds link resources/ into the build tree, so a staged path can resolve to the repository itself.
get_filename_component(_hms_real "${HMS_DIR}" REALPATH)
get_filename_component(_source_hms_real "${CMAKE_CURRENT_LIST_DIR}/../../resources/hms" REALPATH)
if(_hms_real STREQUAL _source_hms_real)
    message(FATAL_ERROR "PackHmsResources: refusing to zip the source tree (${_hms_real}); pass a staged copy")
endif()

# Verified with CMake 3.18 and later. Older toolchains ship the plain JSON, which HMSQuery reads as well.
if(CMAKE_VERSION VERSION_LESS 3.18)
    message(WARNING "PackHmsResources: CMake ${CMAKE_VERSION} < 3.18, shipping HMS files uncompressed")
    return()
endif()

file(GLOB _hms_files LIST_DIRECTORIES false RELATIVE "${HMS_DIR}" "${HMS_DIR}/hms_*.json")
if(NOT _hms_files)
    if(EXISTS "${HMS_DIR}/hms.zip")
        message(STATUS "PackHmsResources: ${HMS_DIR} is already zipped")
    else()
        message(WARNING "PackHmsResources: no HMS files to zip in ${HMS_DIR}")
    endif()
    return()
endif()
list(SORT _hms_files) # stable entry order across platforms and file systems

# Archive paths are stored as given, so run from HMS_DIR to keep the entry names bare (hms_en_31B.json).
# No --mtime: a date is parsed in local time, and east of UTC 1970-01-01 is before the epoch and is rejected.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf hms.zip --format=zip -- ${_hms_files}
    WORKING_DIRECTORY "${HMS_DIR}"
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    file(REMOVE "${HMS_DIR}/hms.zip")
    message(FATAL_ERROR "PackHmsResources: failed to create ${HMS_DIR}/hms.zip, see the CMake error above")
endif()
foreach(_name IN LISTS _hms_files)
    file(REMOVE "${HMS_DIR}/${_name}")
endforeach()

list(LENGTH _hms_files _count)
message(STATUS "PackHmsResources: zipped ${_count} HMS files into ${HMS_DIR}/hms.zip")
