cmake_minimum_required(VERSION 3.13)

set(_python_abi "312")

foreach(_var PYTHON_SOURCE_DIR PYTHON_BUILD_DIR PYTHON_DEST_DIR PYTHON_LAYOUT_ARCH)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()

set(_python_exe "${PYTHON_BUILD_DIR}/python.exe")
if(PYTHON_DEBUG)
    set(_python_exe "${PYTHON_BUILD_DIR}/python_d.exe")
endif()

if(NOT EXISTS "${_python_exe}")
    message(FATAL_ERROR "Built Python executable not found: ${_python_exe}")
endif()

file(REMOVE_RECURSE "${PYTHON_DEST_DIR}")
file(MAKE_DIRECTORY "${PYTHON_DEST_DIR}")

# CPython's Windows layout helper reads LICENSE.txt from the build output.
# Source archives ship this file as LICENSE, so provide the expected name.
if(EXISTS "${PYTHON_SOURCE_DIR}/LICENSE" AND NOT EXISTS "${PYTHON_BUILD_DIR}/LICENSE.txt")
    configure_file("${PYTHON_SOURCE_DIR}/LICENSE" "${PYTHON_BUILD_DIR}/LICENSE.txt" COPYONLY)
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
            "PYTHONHOME="
            "PYTHONPATH=${PYTHON_SOURCE_DIR}/Lib"
            "${_python_exe}"
            "${PYTHON_SOURCE_DIR}/PC/layout"
            --source "${PYTHON_SOURCE_DIR}"
            --build "${PYTHON_BUILD_DIR}"
            --arch "${PYTHON_LAYOUT_ARCH}"
            --copy "${PYTHON_DEST_DIR}"
            --include-dev
    WORKING_DIRECTORY "${PYTHON_SOURCE_DIR}"
    RESULT_VARIABLE _layout_result
)

if(NOT _layout_result EQUAL 0)
    message(FATAL_ERROR "CPython Windows layout staging failed with exit code ${_layout_result}")
endif()

set(_required_files
    "${PYTHON_DEST_DIR}/Lib/encodings/__init__.py"
    "${PYTHON_DEST_DIR}/include/Python.h"
)

if(PYTHON_DEBUG)
    list(APPEND _required_files
        "${PYTHON_DEST_DIR}/python_d.exe"
        "${PYTHON_DEST_DIR}/python${_python_abi}_d.dll"
        "${PYTHON_DEST_DIR}/libs/python${_python_abi}_d.lib"
    )
else()
    list(APPEND _required_files
        "${PYTHON_DEST_DIR}/python.exe"
        "${PYTHON_DEST_DIR}/python${_python_abi}.dll"
        "${PYTHON_DEST_DIR}/libs/python${_python_abi}.lib"
    )
endif()

foreach(_required_file IN LISTS _required_files)
    if(NOT EXISTS "${_required_file}")
        message(FATAL_ERROR "Staged Python file missing: ${_required_file}")
    endif()
endforeach()
