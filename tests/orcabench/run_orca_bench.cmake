# Runs orca_bench with ARGS and fails unless it exits with EXIT and, when OUTPUT is set, prints
# something matching it.
execute_process(COMMAND "${EXE}" ${ARGS} RESULT_VARIABLE exit_code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if (NOT exit_code STREQUAL "${EXIT}")
    message(FATAL_ERROR "orca_bench ${ARGS} exited with ${exit_code}, expected ${EXIT}:\n${out}${err}")
endif ()
if (DEFINED OUTPUT AND NOT "${out}${err}" MATCHES "${OUTPUT}")
    message(FATAL_ERROR "orca_bench ${ARGS} printed nothing matching \"${OUTPUT}\":\n${out}${err}")
endif ()
