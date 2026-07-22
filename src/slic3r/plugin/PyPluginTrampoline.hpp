#ifndef slic3r_PyPluginTrampoline_hpp_
#define slic3r_PyPluginTrampoline_hpp_

#include <pybind11/embed.h>

#include <boost/log/trivial.hpp>

#include <optional>
#include <stdexcept>

#include "PythonPluginInterface.hpp"
#include "PythonInterpreter.hpp"
#include "PluginFsUtils.hpp"
#include "PluginAuditManager.hpp"

// Trampoline variants of pybind11's override macros. Every C++->Python plugin call crosses a
// trampoline method, so this single boundary is where we (1) log the full Python traceback and
// rethrow the exception intact, and (2) open the plugin's filesystem audit scope for the call.
// We catch ONLY error_already_set (a Python-side raise); other pybind11_fail/runtime_error, such as
// a pure-virtual-missing failure, must keep their own path and are deliberately not caught here.

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

// Opens the plugin's filesystem audit scope for the duration of a C++ -> Python call, and publishes
// the calling capability's cached name so host APIs invoked from Python can tell which capability
// they are serving. No-op without an audit plugin key. Declares a local `_orca_audit_scope`.
#define ORCA_PY_AUDIT_SCOPE(mode) \
    std::optional<::Slic3r::ScopedPluginAuditContext> _orca_audit_scope; \
    if (const std::string& _orca_audit_key = this->audit_plugin_key(); !_orca_audit_key.empty()) \
    _orca_audit_scope.emplace(_orca_audit_key, this->name(), mode)

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

    std::string get_name() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE_PURE, std::string, Base, get_name);
    }

    // Config UI hooks. Available on every capability type, so they live here rather than in
    // PyPluginInterfaceTrampoline. A Python exception is rethrown; the caller decides the fallback.
    bool has_config_ui() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [] {},
            PYBIND11_OVERRIDE,
            bool,
            Base,
            has_config_ui);
    }

    std::string get_config_ui() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [] {},
            PYBIND11_OVERRIDE,
            std::string,
            Base,
            get_config_ui);
    }

    // Hand-rolled rather than PYBIND11_OVERRIDE: the macro casts the Python result to the return
    // type, and nlohmann::json has no pybind caster (config crosses this boundary through the
    // explicit py_to_json/json_to_py helpers). Otherwise identical — same audit scope, same rethrow.
    //
    // The hook is optional, and "not implemented" must mean an EMPTY config: no override, or an
    // override returning None or any non-object (`def get_default_config(self): pass` is the easy
    // mistake), both fall back to the base's empty object rather than writing `"cap_config": null`.
    nlohmann::json get_default_config() const override
    {
        ORCA_PY_AUDIT_SCOPE(::Slic3r::PluginAuditManager::AuditMode::Loading);
        try {
            pybind11::gil_scoped_acquire gil;
            pybind11::function override = pybind11::get_override(static_cast<const Base*>(this), "get_default_config");
            if (!override)
                return Base::get_default_config();

            nlohmann::json config = ::Slic3r::py_to_json(override());
            if (!config.is_object()) {
                BOOST_LOG_TRIVIAL(warning)
                    << "Plugin capability '" << this->name() << "' of plugin '" << this->audit_plugin_key()
                    << "': get_default_config() returned " << config.type_name() << ", not an object; restoring an empty config";
                return Base::get_default_config();
            }
            return config;
        } catch (pybind11::error_already_set& err) {
            ::Slic3r::log_python_exception_keep(err);
            throw;
        }
    }

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

    PluginCapabilityType get_type() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading, [] {}, PYBIND11_OVERRIDE, PluginCapabilityType, PluginCapabilityInterface,
            get_type);
    }
};
} // namespace Slic3r

#endif
