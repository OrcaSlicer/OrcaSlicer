#pragma once

// Shared embedded-interpreter bootstrap for slic3rutils tests that need a live Python
// interpreter (test_plugin_host_api.cpp, test_slicing_pipeline_bindings.cpp, ...).
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <memory.h>
#include <stdexcept>
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <slic3r/plugin/PythonPluginBridge.hpp>

namespace {

void ensure_python_initialized()
{
    if (Py_IsInitialized())
        return;

    static std::unique_ptr<pybind11::scoped_interpreter> interpreter;

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;

    const auto python_home = boost::dll::program_location().parent_path() / "python";

    if (boost::filesystem::exists(python_home)) {
        const std::string home = python_home.string();
        const PyStatus status  = PyConfig_SetBytesString(&config, &config.home, home.c_str());

        if (PyStatus_Exception(status)) {
            const char* message = status.err_msg ? status.err_msg : "Failed to set Python home";
            PyConfig_Clear(&config);
            throw std::runtime_error(message);
        }
    }

    interpreter = std::make_unique<pybind11::scoped_interpreter>(&config);
}

pybind11::module_ import_orca_module()
{
    ensure_python_initialized();

    // Force PythonPluginBridge.cpp into the test binary so the embedded
    // PYBIND11_EMBEDDED_MODULE(orca, ...) registration is available.
    (void) Slic3r::PythonPluginBridge::instance();
    return pybind11::module_::import("orca");
}

} // namespace
