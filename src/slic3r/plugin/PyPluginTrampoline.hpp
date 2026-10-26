#ifndef slic3r_PyPluginTrampoline_hpp_
#define slic3r_PyPluginTrampoline_hpp_

#include "libslic3r/LifecycleEvents.hpp"
#include <exception>
#include <pybind11/embed.h>

#include <boost/log/trivial.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "PythonPluginInterface.hpp"
#include "PythonInterpreter.hpp"
#include "PluginFsUtils.hpp"
#include "PluginAuditManager.hpp"
#include "pybind11/cast.h"
#include <pybind11/pybind11.h>
#include <pybind11/gil.h>
#include <pybind11/pytypes.h>
#include <utility>

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
        const bool _orca_audit_denial = ::Slic3r::PluginAuditManager::instance().audit_denial_pending(); \
        if (_orca_audit_denial) \
            ::Slic3r::PluginAuditManager::instance().clear_current_plugin(); \
        ::Slic3r::log_python_exception_keep(err); \
        if (_orca_audit_denial) \
            ::Slic3r::PluginAuditManager::instance().clear_audit_denial(); \
        throw; \
    }

// Opens the plugin's filesystem audit scope for the duration of a C++ -> Python call, and publishes
// the calling capability's cached name so host APIs invoked from Python can tell which capability
// they are serving. No-op without an audit plugin key. Declares a local `_orca_audit_scope`.
#define ORCA_PY_AUDIT_SCOPE() \
    std::optional<::Slic3r::ScopedPluginAuditContext> _orca_audit_scope; \
    if (const std::string& _orca_audit_key = this->audit_plugin_key(); !_orca_audit_key.empty()) \
    _orca_audit_scope.emplace(_orca_audit_key, this->name())

#define ORCA_PY_OVERRIDE_AUDITED(audit_setup, override_macro, ret, base, name, ...) \
    do { \
        ::Slic3r::PluginCapabilityInterface::RefCounter _orca_ref_counter(*this); \
        ::Slic3r::PythonGILState _orca_python_gil; \
        if (!_orca_python_gil) \
            throw std::runtime_error("Python interpreter is shutting down"); \
        ORCA_PY_AUDIT_SCOPE(); \
        if (_orca_audit_scope) \
            audit_setup(); \
        ORCA_PY_LOGGED_OVERRIDE_BODY(override_macro(ret, base, name, ##__VA_ARGS__)); \
    } while (0)

namespace Slic3r {

// True when `object` is a coroutine object (the value produced by calling an `async def`).
// Match inspect.iscoroutine() without importing Python modules inside the plugin audit scope.
// Must be called with the GIL held.
inline bool py_is_coroutine(const pybind11::handle& object)
{
    return object && PyCoro_CheckExact(object.ptr());
}

// Result of invoking a Script capability's Python `execute` exactly once.
//
// A synchronous override (`def execute`) runs to completion here and yields `result`.
// An asynchronous override (`async def execute`, or any override that returns a coroutine)
// yields the coroutine object in `awaitable` WITHOUT running it: the cast to ExecutionResult
// that the trampoline would otherwise perform is impossible for a coroutine, so the caller
// chooses how to drive it. Whoever drives it must keep the capability referenced and re-open
// this call's GIL + runtime lease + audit scope for the coroutine's whole lifetime (see
// ScriptAsyncRuntime in PluginManager.hpp).
struct ScriptExecuteInvocation
{
    bool             is_async = false;
    pybind11::object awaitable; // valid iff is_async; a coroutine object created under the GIL
    ExecutionResult  result;    // valid iff !is_async
    std::string      error;     // non-empty when the call or coroutine creation raised
};

// Invokes ScriptPluginCapability::execute() under the GIL, replicating the audited trampoline
// call (capability ref counter, audit scope, traceback-preserving rethrow) but returning a
// coroutine object instead of forcing a cast when the plugin's `execute` is `async def`.
//
// `capability` must be the concrete, Python-bound capability instance (obtainable from
// get_plugin_capability()); it is resolved back to its Python object through pybind's instance
// map. Never throws: a Python exception is logged (full traceback) and reported in `error`.
inline ScriptExecuteInvocation invoke_script_execute(const std::shared_ptr<PluginCapabilityInterface>& capability)
{
    ScriptExecuteInvocation invocation;
    if (!capability)
        return invocation;

    try {
        pybind11::gil_scoped_acquire gil;
        PluginCapabilityInterface::RefCounter ref_counter(*capability);

        std::optional<ScopedPluginAuditContext> audit_scope;
        if (const std::string& audit_key = capability->audit_plugin_key(); !audit_key.empty())
            audit_scope.emplace(audit_key, capability->name());

        pybind11::object py_capability = pybind11::cast(capability);
        pybind11::object returned      = py_capability.attr("execute")();

        if (py_is_coroutine(returned)) {
            invocation.is_async = true;
            invocation.awaitable = std::move(returned);
            return invocation;
        }

        invocation.result = returned.cast<ExecutionResult>();
    } catch (pybind11::error_already_set& err) {
        const bool audit_denial = PluginAuditManager::instance().audit_denial_pending();
        if (audit_denial)
            PluginAuditManager::instance().clear_current_plugin();
        invocation.error = err.what();
        log_python_exception_keep(err);
        if (audit_denial)
            PluginAuditManager::instance().clear_audit_denial();
    } catch (const std::exception& ex) {
        invocation.error = ex.what();
    }

    return invocation;
}

template<class Base> class PyPluginCommonTrampoline : public Base
{
public:
    using Base::Base;

    std::string get_name() const override
    {
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE_PURE, std::string, Base, get_name);
    }

    // Config UI hooks. Available on every capability type, so they live here rather than in
    // PyPluginInterfaceTrampoline. A Python exception is rethrown; the caller decides the fallback.
    bool has_config_ui() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE,
            bool,
            Base,
            has_config_ui);
    }

    std::string get_config_ui() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
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
        pybind11::gil_scoped_acquire gil;
        ORCA_PY_AUDIT_SCOPE();
        try {
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
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE, void, Base, on_load);
    }

    void on_unload() override
    {
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE, void, Base, on_unload);
    }

    void on_cancelled() override
    {
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE, void, Base, on_cancelled);
    }

    void on_lifecycle_event(LifecycleEvent event, const LifecycleEventContext& ctx) override
    {
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE, void, Base, on_lifecycle_event, event, ctx);
    }
};

class PyPluginInterfaceTrampoline : public PyPluginCommonTrampoline<PluginCapabilityInterface>
{
public:
    using PyPluginCommonTrampoline<PluginCapabilityInterface>::PyPluginCommonTrampoline;

    PluginCapabilityType get_type() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, PluginCapabilityType, PluginCapabilityInterface,
            get_type);
    }
};
} // namespace Slic3r

#endif
