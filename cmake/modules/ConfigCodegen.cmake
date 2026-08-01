# OrcaSlicer Config Codegen CMake Module
#
# Generates C++ source files from protobuf schema definitions.
# Generated files live in src/slic3r/GUI/generated/ and are gitignored.
# Run 'python tools/run_codegen.py' to regenerate; it resolves protoc and the Python
# packages it needs itself (see tools/codegen_toolchain.py).
#
# Targets:
#   codegen_config  - Custom target to regenerate C++ from .proto files
#   validate_config - Custom target to validate generated vs original
#
# Usage in parent CMakeLists.txt:
#   include(cmake/modules/ConfigCodegen.cmake)

find_program(PROTOC_EXECUTABLE protoc)
find_package(Python3 COMPONENTS Interpreter QUIET)

set(_generated_marker "${CMAKE_SOURCE_DIR}/src/slic3r/GUI/generated/PrintConfigDef_generated.cpp")

# Decide which interpreter can actually run the codegen.
#
# The main CMakeLists forces Python3_EXECUTABLE to the *bundled embed* interpreter (used for
# the in-app Python plugin runtime). That interpreter has neither protoc nor the protobuf /
# pyyaml packages, and for cross-compiled targets it may not even be the host architecture — so
# it cannot run tools/run_codegen.py. Prefer a host interpreter, which tools/run_codegen.py can
# bootstrap the toolchain into.
#
# ORCA_CODEGEN_PYTHON lets a build explicitly point at the interpreter to use.
if(NOT ORCA_CODEGEN_PYTHON)
    find_program(_orca_host_python NAMES python3 python)
    if(_orca_host_python)
        set(ORCA_CODEGEN_PYTHON "${_orca_host_python}")
    else()
        set(ORCA_CODEGEN_PYTHON "${Python3_EXECUTABLE}")
    endif()
endif()

# --check answers "can this interpreter regenerate right now?" (protobuf + pyyaml importable
# and a protoc already resolvable). It never downloads or installs, so wiring up the build-time
# regeneration below never turns a build into a network operation.
set(_codegen_usable FALSE)
if(ORCA_CODEGEN_PYTHON)
    execute_process(
        COMMAND ${ORCA_CODEGEN_PYTHON} "${CMAKE_SOURCE_DIR}/tools/codegen_toolchain.py" --check
        RESULT_VARIABLE _codegen_probe
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_codegen_probe EQUAL 0)
        set(_codegen_usable TRUE)
    endif()
endif()

# If generated files are missing (fresh clone), run the codegen now at configure time so a plain
# cmake configure + build works without a separate pre-build step. Unlike the build-time
# regeneration above this is allowed to fetch the toolchain — there is nothing to build without it.
if(NOT EXISTS "${_generated_marker}")
    set(_codegen_result 1)
    if(ORCA_CODEGEN_PYTHON)
        message(STATUS "Config codegen: generated files missing — running codegen now...")
        execute_process(
            COMMAND ${ORCA_CODEGEN_PYTHON} "${CMAKE_SOURCE_DIR}/tools/run_codegen.py" --no-validate
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE _codegen_result
        )
    endif()
    if(NOT _codegen_result EQUAL 0)
        message(FATAL_ERROR
            "Config codegen: generated files are missing and could not be generated.\n"
            "Run it manually with a host Python:\n"
            "  python tools/run_codegen.py\n"
            "or pass -DORCA_CODEGEN_PYTHON=<python>. Offline builds can point at an installed\n"
            "protoc with PROTOC=<path>.")
    endif()
    message(STATUS "Config codegen: generated files created successfully")
endif()

set(CONFIG_PROTO_DIR   "${CMAKE_SOURCE_DIR}/src/PrintConfigs")
set(CONFIG_CODEGEN_DIR "${CMAKE_SOURCE_DIR}/src/slic3r/GUI/generated")
set(CONFIG_LAYOUT_YAML "${CMAKE_SOURCE_DIR}/src/PrintConfigs/layout.yaml")
set(CONFIG_DESC_FILE   "${CMAKE_BINARY_DIR}/config.desc")

set(CODEGEN_TOOL     "${CMAKE_SOURCE_DIR}/tools/config_codegen.py")
set(VALIDATE_TOOL    "${CMAKE_SOURCE_DIR}/tools/validate_codegen.py")
set(RUN_CODEGEN_TOOL "${CMAKE_SOURCE_DIR}/tools/run_codegen.py")

# Generated output files (TabLayout_generated.cpp is also generated from layout.yaml)
set(CONFIG_GENERATED_SOURCES
    "${CONFIG_CODEGEN_DIR}/PrintConfigDef_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/Preset_options_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/Invalidation_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/OptionKeys_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/TabLayout_generated.cpp"
)
set(CONFIG_GENERATED_SOURCES "${CONFIG_GENERATED_SOURCES}" CACHE INTERNAL "Generated config cpp files")

# Collect all .proto source files (flat in src/PrintConfigs/, excluding config_metadata.proto)
file(GLOB CONFIG_PROTO_FILES
    "${CONFIG_PROTO_DIR}/filament.proto"
    "${CONFIG_PROTO_DIR}/print.proto"
    "${CONFIG_PROTO_DIR}/printer.proto"
)
set(CONFIG_PROTO_FILES
    "${CONFIG_PROTO_DIR}/config_metadata.proto"
    ${CONFIG_PROTO_FILES}
)

if(_codegen_usable)
    # Single command: run_codegen.py resolves protoc itself.
    # Proto files → generated .cpp files. Runs automatically when any .proto changes.
    add_custom_command(
        OUTPUT  ${CONFIG_GENERATED_SOURCES}
        COMMAND ${ORCA_CODEGEN_PYTHON} ${RUN_CODEGEN_TOOL} --no-validate
        DEPENDS ${CONFIG_PROTO_FILES} ${CONFIG_LAYOUT_YAML} ${CODEGEN_TOOL}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Re-generating config C++ from changed .proto files"
        VERBATIM
    )

    # codegen_config is part of ALL — runs before every build, checks if protos changed.
    add_custom_target(codegen_config ALL
        DEPENDS ${CONFIG_GENERATED_SOURCES}
        COMMENT "Config codegen up to date"
    )

    # Validation target: cmake --build . --target validate_config
    add_custom_target(validate_config
        COMMAND ${ORCA_CODEGEN_PYTHON} ${VALIDATE_TOOL}
        DEPENDS ${CONFIG_GENERATED_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Validating generated config code against PrintConfig.cpp"
        VERBATIM
    )

    message(STATUS "Config codegen: enabled — proto changes auto-regenerate on next build")
else()
    # No usable codegen toolchain in this interpreter (e.g. the bundled embed Python in CI).
    # The generated files already exist (checked/produced above); use them as-is and provide a
    # no-op codegen_config target so dependents that reference it still resolve.
    add_custom_target(codegen_config ALL
        COMMENT "Config codegen: using pre-generated files (no codegen toolchain in this interpreter)")
    add_custom_target(validate_config
        COMMENT "Config codegen: validation skipped (no codegen toolchain in this interpreter)")
    message(STATUS "Config codegen: no codegen toolchain in '${ORCA_CODEGEN_PYTHON}' — using pre-generated files")
endif()
