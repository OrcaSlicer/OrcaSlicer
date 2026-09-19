#pragma once

// Process-wide embedded-interpreter bootstrap for slic3rutils tests. Tests that need the full
// PluginManager lifecycle call release_test_python_interpreter() before taking ownership through
// PythonInterpreter; direct binding tests use ensure_python_initialized()/import_orca_module().
#include <pybind11/pybind11.h>

void ensure_python_initialized();
void release_test_python_interpreter() noexcept;
pybind11::module_ import_orca_module();
