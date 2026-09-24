#pragma once

#include <pybind11/pybind11.h>

namespace Slic3r::host_bindings {

// Compiled only with SLIC3R_ENABLE_PLUGIN_HOST_TEST_API. This is intentionally
// absent from distributed Plugin Host builds.
void register_test_support(pybind11::module_& host);

} // namespace Slic3r::host_bindings
