#ifndef slic3r_PyPluginTrampoline_hpp_
#define slic3r_PyPluginTrampoline_hpp_

#include <pybind11/embed.h>

#include <optional>
#include <stdexcept>

#include "PythonPluginInterface.hpp"
#include "PythonInterpreter.hpp"
#include "PluginAuditManager.hpp"

// Trampoline variants of pybind11's override macros. Every C++->Python plugin call
// crosses through a trampoline method, so this single boundary is where we (1) log the
// full Python traceback (to sys.stderr -> session log) and rethrow the exception intact,
// and (2) open the plugin's filesystem audit scope for the duration of the call.
// We catch ONLY error_already_set (a Python-side raise); other pybind11_fail/runtime_error
// like a pure-virtual-missing failure must keep their own path and are deliberately not
// caught here.

// Logs (and rethrows) a Python exception from a pybind11 override call, preserving the
// traceback. Internal helper shared by the public macros below and by trampolines that
// manage their own audit scope (e.g. the G-code plugin).
#define ORCA_PY_LOGGED_OVERRIDE_BODY(override_call) \
    try { \
        override_call; \
    } catch (pybind11::error_already_set & err) { \
        ::Slic3r::log_python_exception_keep(err); \
        throw; \
    }

// Opens the plugin's filesystem audit scope for the duration of a C++ -> Python call
// when this trampoline instance carries a non-empty audit plugin key. Declares a local
// `_orca_audit_scope`.
#define ORCA_PY_AUDIT_SCOPE(mode) \
    std::optional<::Slic3r::ScopedPluginAuditContext> _orca_audit_scope; \
    if (const std::string& _orca_audit_key = this->audit_plugin_key(); !_orca_audit_key.empty()) \
    _orca_audit_scope.emplace(_orca_audit_key, mode)

#define ORCA_PY_OVERRIDE_AUDITED(mode, audit_setup, override_macro, ret, base, name, ...) \
    do { \
        ::Slic3r::PluginCapabilityInterface::RefCounter _orca_ref_counter(*this); \
        ::Slic3r::PythonGILState _orca_python_gil; \
        if (!_orca_python_gil) \
            throw std::runtime_error("Python interpreter is shutting down"); \
        ORCA_PY_AUDIT_SCOPE(mode); \
        if (_orca_audit_scope) \
            audit_setup(); \
        ORCA_PY_LOGGED_OVERRIDE_BODY(override_macro(ret, base, name, ##__VA_ARGS__)); \
    } while (0)

namespace Slic3r {
template<class Base> class PyPluginCommonTrampoline : public Base
{
public:
    using Base::Base;

    // get_name is required on all capabilities — Python subclass must implement it.
    std::string get_name() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, std::string, Base, get_name);
    }

    // All plugins may define their own on_load/unload functions.
    void on_load() override
    {
        ORCA_PY_OVERRIDE_AUDITED(::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE, void, Base, on_load);
    }

    void on_unload() override
    {
        ORCA_PY_OVERRIDE_AUDITED(::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE, void, Base, on_unload);
    }

    void on_cancelled() override
    {
        ORCA_PY_OVERRIDE_AUDITED(::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE, void, Base, on_cancelled);
    }
};

class PyPluginInterfaceTrampoline : public PyPluginCommonTrampoline<PluginCapabilityInterface>
{
public:
    using PyPluginCommonTrampoline<PluginCapabilityInterface>::PyPluginCommonTrampoline;

    // get_name is implemented in PyPluginCommonTrampoline (PYBIND11_OVERRIDE_PURE).

    PluginCapabilityType get_type() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE, PluginCapabilityType, PluginCapabilityInterface,
            get_type);
    }
};
} // namespace Slic3r

#endif
