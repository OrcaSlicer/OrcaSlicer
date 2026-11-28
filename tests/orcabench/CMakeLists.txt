get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
    test_buildid.cpp
    test_catalog.cpp
    test_compare.cpp
    test_document.cpp
    test_host.cpp
    test_measurement.cpp
    test_options.cpp
    test_policy.cpp
    test_reporters.cpp
    test_result.cpp
    test_runner.cpp
    test_sampler.cpp
    test_summary.cpp
    test_workload.cpp
    )

target_link_libraries(${_TEST_NAME}_tests test_common orcabench_core Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

# A build with neither a git checkout nor the git_commit_hash variable, such as a source tarball,
# records GitCommitHash.cmake's placeholder, and the revision test skips there.
find_package(Git QUIET)
set(_has_commit 0)
if ((GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git") OR NOT "$ENV{git_commit_hash}" STREQUAL "")
    set(_has_commit 1)
endif ()
target_compile_definitions(${_TEST_NAME}_tests PRIVATE ORCABENCH_HAS_COMMIT=${_has_commit})

orcaslicer_discover_tests(${_TEST_NAME}_tests)

# The tests that slice, apart so the suite above builds without libslic3r and runs in about a second.
add_executable(${_TEST_NAME}_slicer_tests
    test_environment.cpp
    test_fixtures.cpp
    test_load.cpp
    test_slice.cpp
    )
target_link_libraries(${_TEST_NAME}_slicer_tests test_common orcabench_slicer Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_slicer_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls(${_TEST_NAME}_slicer_tests)
# A pass stuck waiting on TBB fails after five minutes, well inside the unit-test job's own limit.
orcaslicer_discover_tests(${_TEST_NAME}_slicer_tests PROPERTIES TIMEOUT 300)

# orca_bench builds outside build/tests, which is all the CI unit-test job receives, and
# run_orca_bench.cmake checks its exit code as well as its output.
set(_run_orca_bench "${CMAKE_CURRENT_SOURCE_DIR}/run_orca_bench.cmake")
add_test(NAME orca_bench_lists_the_catalog
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> -DARGS=--list -DEXIT=0 -DOUTPUT=slice/smoke-cube/standard
                 -P ${_run_orca_bench})
add_test(NAME orca_bench_refuses_an_unknown_argument
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> -DARGS=--bogus -DEXIT=2
                 "-DOUTPUT=unknown argument '--bogus'" -P ${_run_orca_bench})
add_test(NAME orca_bench_prints_the_usage_for_help_beside_list
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> -DARGS=--list$<SEMICOLON>--help -DEXIT=0
                 "-DOUTPUT=usage: orca_bench" -P ${_run_orca_bench})
# result_v1_changed.json ran at other threads, and changed the idler's output.
set(_before "${CMAKE_SOURCE_DIR}/tests/data/orcabench/result_v1.json")
set(_after "${CMAKE_SOURCE_DIR}/tests/data/orcabench/result_v1_changed.json")
add_test(NAME orca_bench_compares_two_results
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--compare$<SEMICOLON>${_before}$<SEMICOLON>${_before}" -DEXIT=0
                 "-DOUTPUT=1 compared, 0 with changed output" -P ${_run_orca_bench})
add_test(NAME orca_bench_refuses_results_that_measured_differently
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--compare$<SEMICOLON>${_before}$<SEMICOLON>${_after}" -DEXIT=1
                 "-DOUTPUT=pass --allow-mismatch to compare them anyway" -P ${_run_orca_bench})
add_test(NAME orca_bench_exits_3_when_output_changed
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench>
                 "-DARGS=--compare$<SEMICOLON>${_before}$<SEMICOLON>${_after}$<SEMICOLON>--allow-mismatch" -DEXIT=3
                 "-DOUTPUT=OUTPUT CHANGED in 1 workload" -P ${_run_orca_bench})
# result_v1_failed.json failed the idler, which result_v1.json ran.
set(_failed "${CMAKE_SOURCE_DIR}/tests/data/orcabench/result_v1_failed.json")
add_test(NAME orca_bench_exits_1_when_a_workload_failed_only_in_b
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--compare$<SEMICOLON>${_before}$<SEMICOLON>${_failed}" -DEXIT=1
                 "-DOUTPUT=1 failed only in b" -P ${_run_orca_bench})
set_tests_properties(orca_bench_exits_1_when_a_workload_failed_only_in_b PROPERTIES LABELS RequiresApp)
add_test(NAME orca_bench_names_a_result_it_cannot_read
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--compare$<SEMICOLON>${_before}$<SEMICOLON>${_before}.missing"
                 -DEXIT=1 "-DOUTPUT=cannot read .*result_v1.json.missing" -P ${_run_orca_bench})
add_test(NAME orca_bench_needs_two_results_to_compare
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--compare$<SEMICOLON>${_before}" -DEXIT=2
                 "-DOUTPUT=--compare needs a.json b.json" -P ${_run_orca_bench})
# A run of the smoke cube writes a result, which compares against itself unchanged and is then
# removed, so the comparison never reads a file an earlier run left.
set(_smoke_result "${CMAKE_CURRENT_BINARY_DIR}/smoke_cube_run.json")
string(JOIN "$<SEMICOLON>" _smoke_run --policy quick --filter slice/smoke-cube/* --out ${_smoke_result})
add_test(NAME orca_bench_runs_a_workload_and_writes_its_result
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_smoke_run}" -DEXIT=0
                 "-DOUTPUT=1 ran, 0 skipped, 0 failed" -P ${_run_orca_bench})
add_test(NAME orca_bench_compares_a_run_with_itself
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--compare$<SEMICOLON>${_smoke_result}$<SEMICOLON>${_smoke_result}"
                 -DEXIT=0 "-DOUTPUT=1 compared, 0 with changed output" -P ${_run_orca_bench})
add_test(NAME orca_bench_removes_the_run_result COMMAND ${CMAKE_COMMAND} -E rm -f ${_smoke_result})
set_tests_properties(orca_bench_runs_a_workload_and_writes_its_result PROPERTIES FIXTURES_SETUP smoke_cube_run)
set_tests_properties(orca_bench_compares_a_run_with_itself PROPERTIES FIXTURES_REQUIRED smoke_cube_run)
set_tests_properties(orca_bench_removes_the_run_result PROPERTIES FIXTURES_CLEANUP smoke_cube_run)
# An out file in a directory that does not exist fails before the run, so no workload starts.
string(JOIN "$<SEMICOLON>" _unwritable_out --policy quick --filter slice/smoke-cube/* --out ${CMAKE_CURRENT_BINARY_DIR}/missing/run.json)
add_test(NAME orca_bench_refuses_an_out_file_it_cannot_write_before_the_run
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_unwritable_out}" -DEXIT=1
                 "-DOUTPUT=^orca_bench: cannot write .*missing/run[.]json[^a-z]*$" -P ${_run_orca_bench})
set_tests_properties(orca_bench_refuses_an_out_file_it_cannot_write_before_the_run PROPERTIES LABELS RequiresApp)
add_test(NAME orca_bench_refuses_an_unknown_policy
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--policy$<SEMICOLON>fastest" -DEXIT=2
                 "-DOUTPUT=unknown policy 'fastest'" -P ${_run_orca_bench})
add_test(NAME orca_bench_names_a_filter_that_matches_nothing
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=--list$<SEMICOLON>--filter$<SEMICOLON>slice/nothing/*"
                 -DEXIT=1 "-DOUTPUT=no workload in the catalog matches --filter 'slice/nothing/[*]'" -P ${_run_orca_bench})
string(JOIN "$<SEMICOLON>" _procedural_loads --list --kind load --tag procedural)
add_test(NAME orca_bench_lists_the_workloads_of_a_kind_and_tag
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_procedural_loads}"
                 -DEXIT=0 "-DOUTPUT=^load/fine-sphere/stl[^a-z]*$" -P ${_run_orca_bench})
# Nothing may follow the document's closing brace, as the progress line would.
string(JOIN "$<SEMICOLON>" _smoke_json --policy quick --filter slice/smoke-cube/* --reporter json --quiet)
add_test(NAME orca_bench_prints_a_quiet_run_as_its_result_document
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_smoke_json}" -DEXIT=0
                 "-DOUTPUT=^[{].*\"workloads\".*[}][^a-z]*$" -P ${_run_orca_bench})
# Verbose rows name their scope, and by start posSlice comes before export, which by time comes first.
string(JOIN "$<SEMICOLON>" _smoke_layout --policy quick --filter slice/smoke-cube/* --iterations 1 --verbose --sort-by start)
add_test(NAME orca_bench_runs_with_the_layout_flags
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_smoke_layout}" -DEXIT=0
                 "-DOUTPUT=posSlice object:0.*psGCodeExport print" -P ${_run_orca_bench})
set_tests_properties(orca_bench_runs_with_the_layout_flags PROPERTIES LABELS RequiresApp)
string(JOIN "$<SEMICOLON>" _compare_layout --compare ${_before} ${_before} --verbose --sort-by start --collapse-below 0)
add_test(NAME orca_bench_compares_with_the_layout_flags
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_compare_layout}"
                 -DEXIT=0 "-DOUTPUT=posSlice object:0.*psGCodeExport print" -P ${_run_orca_bench})
# A determinism check that keeps its G-code, whose second pass a second test reads.
set(_smoke_dump "${CMAKE_CURRENT_BINARY_DIR}/smoke_cube_dump")
string(JOIN "$<SEMICOLON>" _smoke_determinism --check-determinism --filter slice/smoke-cube/* --dump-gcode ${_smoke_dump})
add_test(NAME orca_bench_checks_that_a_workload_is_deterministic
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_smoke_determinism}"
                 -DEXIT=0 "-DOUTPUT=orca_bench  verify.*hash [0-9a-f]+.*1 ran, 0 skipped, 0 failed" -P ${_run_orca_bench})
add_test(NAME orca_bench_keeps_the_second_pass_g_code
         COMMAND ${CMAKE_COMMAND} -E cat ${_smoke_dump}/slice/smoke-cube/standard/2.gcode)
add_test(NAME orca_bench_removes_the_g_code_it_kept COMMAND ${CMAKE_COMMAND} -E rm -rf ${_smoke_dump})
set_tests_properties(orca_bench_checks_that_a_workload_is_deterministic PROPERTIES FIXTURES_SETUP smoke_cube_dump)
set_tests_properties(orca_bench_keeps_the_second_pass_g_code PROPERTIES FIXTURES_REQUIRED smoke_cube_dump)
set_tests_properties(orca_bench_removes_the_g_code_it_kept PROPERTIES FIXTURES_CLEANUP smoke_cube_dump)
set_tests_properties(orca_bench_keeps_the_second_pass_g_code orca_bench_removes_the_g_code_it_kept PROPERTIES LABELS RequiresApp)
# Timing load and process leaves export untimed and unrun, so the slice writes no G-code to hash.
string(JOIN "$<SEMICOLON>" _smoke_overrides --policy quick --filter slice/smoke-cube/* --iterations 1 --warmup 0 --stages load,process)
add_test(NAME orca_bench_runs_with_the_policy_overridden
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:orca_bench> "-DARGS=${_smoke_overrides}" -DEXIT=0
                 "-DOUTPUT=1 iteration.*hash none" -P ${_run_orca_bench})
set_tests_properties(orca_bench_lists_the_catalog orca_bench_refuses_an_unknown_argument
                     orca_bench_prints_the_usage_for_help_beside_list orca_bench_compares_two_results
                     orca_bench_refuses_results_that_measured_differently orca_bench_exits_3_when_output_changed
                     orca_bench_names_a_result_it_cannot_read orca_bench_needs_two_results_to_compare
                     orca_bench_runs_a_workload_and_writes_its_result orca_bench_compares_a_run_with_itself
                     orca_bench_removes_the_run_result orca_bench_refuses_an_unknown_policy
                     orca_bench_names_a_filter_that_matches_nothing orca_bench_lists_the_workloads_of_a_kind_and_tag
                     orca_bench_runs_with_the_policy_overridden orca_bench_prints_a_quiet_run_as_its_result_document
                     orca_bench_compares_with_the_layout_flags orca_bench_checks_that_a_workload_is_deterministic
                     PROPERTIES LABELS RequiresApp)

# GitCommitHash.cmake lives outside build/tests too, and git may be missing where tests run.
add_test(NAME git_commit_hash_counts_an_untracked_kind
         COMMAND ${CMAKE_COMMAND} -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/modules/GitCommitHash.cmake
                 -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/git_commit_hash_probe
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/test_git_commit_hash.cmake)
set_tests_properties(git_commit_hash_counts_an_untracked_kind PROPERTIES LABELS RequiresApp
                     SKIP_REGULAR_EXPRESSION "SKIP: ")
