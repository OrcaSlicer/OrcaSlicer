#include "python_test_support.hpp"

#include <slic3r/plugin/PythonPluginBridge.hpp>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <pybind11/embed.h>

#include <memory>
#include <mutex>
#include <stdexcept>

namespace {

struct TestPythonOwner
{
    std::mutex                                      mutex;
    std::unique_ptr<pybind11::scoped_interpreter> interpreter;
};

TestPythonOwner& test_python_owner()
{
    static TestPythonOwner owner;
    return owner;
}

} // namespace

void ensure_python_initialized()
{
    TestPythonOwner& owner = test_python_owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    if (Py_IsInitialized())
        return;

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

    owner.interpreter = std::make_unique<pybind11::scoped_interpreter>(&config);
}

void release_test_python_interpreter() noexcept
{
    TestPythonOwner& owner = test_python_owner();
    std::lock_guard<std::mutex> lock(owner.mutex);
    owner.interpreter.reset();
}

pybind11::module_ import_orca_module()
{
    ensure_python_initialized();

    // Force PythonPluginBridge.cpp into the test binary so the embedded
    // PYBIND11_EMBEDDED_MODULE(orca, ...) registration is available.
    (void) Slic3r::PythonPluginBridge::instance();
    return pybind11::module_::import("orca");
}
