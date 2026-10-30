#pragma once

#include "InfillPluginCapability.hpp"

#include "slic3r/plugin/PyPluginTrampoline.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

// How a Python infill override is invoked. The pre-multi-pattern API passed only the request to
// generate() and no argument to no_sort()/aligned_to_origin(); the current API passes the selected
// pattern ID first. Both remain supported.
enum class InfillOverrideCallStyle
{
    WithPatternId,
    Legacy,
};

namespace infill_override_detail {

// Calls Signature.bind() with `argc` positional placeholders. bind() only validates the signature;
// it never runs the plugin, so this is a capability check rather than a trial call.
inline bool signature_binds(const pybind11::object& bind, int argc)
{
    pybind11::tuple args(argc);
    for (int i = 0; i < argc; ++i)
        args[i] = pybind11::none();
    PyObject* result = PyObject_CallObject(bind.ptr(), args.ptr());
    if (result == nullptr)
        throw pybind11::error_already_set();
    Py_DECREF(result);
    return true;
}

} // namespace infill_override_detail

// Decides which call form an override accepts. Must be called with the GIL held and `inspect`
// already imported (module imports read Python sources; the caller imports it outside the plugin
// audit scope). Never invokes `override`: an exception from plugin logic therefore cannot be
// mistaken for a signature mismatch and the plugin is never called twice.
//
// A callable that accepts `with_pattern_id_args` positional arguments is the current form; one that
// accepts only `legacy_args` is a pre-multi-pattern override. `*args` overrides accept both and are
// treated as the current form. When `inspect.signature` cannot describe the callable, the current
// form is the deliberate fallback: the subsequent call raises its normal error.
inline InfillOverrideCallStyle infill_override_call_style(const pybind11::function& override,
                                                          const pybind11::module_& inspect,
                                                          int with_pattern_id_args,
                                                          int legacy_args)
{
    try {
        const pybind11::object signature = inspect.attr("signature")(override);
        const pybind11::object bind      = signature.attr("bind");
        try {
            if (infill_override_detail::signature_binds(bind, with_pattern_id_args))
                return InfillOverrideCallStyle::WithPatternId;
        } catch (pybind11::error_already_set& err) {
            if (!err.matches(PyExc_TypeError))
                throw;
        }
        try {
            if (infill_override_detail::signature_binds(bind, legacy_args))
                return InfillOverrideCallStyle::Legacy;
        } catch (pybind11::error_already_set& err) {
            if (!err.matches(PyExc_TypeError))
                throw;
        }
        // Neither form binds. Use the current contract so the real call reports the mismatch.
        return InfillOverrideCallStyle::WithPatternId;
    } catch (pybind11::error_already_set& err) {
        // An audit denial is a plugin error, not an uninspectable callable: let the trampoline's
        // normal logging and rethrow path handle it.
        if (PluginAuditManager::instance().audit_denial_pending())
            throw;
        if (err.matches(PyExc_SystemExit) || err.matches(PyExc_KeyboardInterrupt))
            throw;
        // inspect.signature() reports an unsupported callable as TypeError or ValueError. Any other
        // exception (for example a plugin's broken __signature__) is a real error and must surface.
        if (!err.matches(PyExc_TypeError) && !err.matches(PyExc_ValueError))
            throw;
        return InfillOverrideCallStyle::WithPatternId;
    }
}

// Cached once per Python capability instance. Native providers do not use this trampoline.
inline InfillOverrideCallStyle infill_cached_override_call_style(std::atomic<int>& cache,
                                                                 const pybind11::function& override,
                                                                 const pybind11::module_& inspect,
                                                                 int with_pattern_id_args,
                                                                 int legacy_args)
{
    const int cached = cache.load(std::memory_order_acquire);
    if (cached >= 0)
        return static_cast<InfillOverrideCallStyle>(cached);
    const InfillOverrideCallStyle style =
        infill_override_call_style(override, inspect, with_pattern_id_args, legacy_args);
    cache.store(static_cast<int>(style), std::memory_order_release);
    return style;
}

class PyInfillPluginCapabilityTrampoline : public PyPluginCommonTrampoline<InfillPluginCapability>
{
public:
    using PyPluginCommonTrampoline<InfillPluginCapability>::PyPluginCommonTrampoline;

    // Optional multi-pattern declaration. PYBIND11_OVERRIDE (not _PURE) keeps the C++ default of one
    // sparse pattern named after the capability when the plugin does not implement patterns().
    std::vector<InfillPluginPattern> patterns() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE,
            std::vector<InfillPluginPattern>,
            InfillPluginCapability,
            patterns);
    }

    // Optional Fill semantics. PYBIND11_OVERRIDE (not _PURE) keeps the C++ false default when the
    // Python class does not define the method. The legacy no-argument form is accepted as well.
    bool no_sort(const std::string& pattern_id) const override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                          python_gil;
        if (!python_gil)
            throw std::runtime_error("Python interpreter is shutting down");
        const bool      resolve_style = m_no_sort_style.load(std::memory_order_acquire) < 0;
        pybind11::module_ inspect;
        if (resolve_style)
            inspect = pybind11::module_::import("inspect");
        ORCA_PY_AUDIT_SCOPE();

        ORCA_PY_LOGGED_OVERRIDE_BODY({
            pybind11::function override =
                pybind11::get_override(static_cast<const InfillPluginCapability*>(this), "no_sort");
            if (!override)
                return InfillPluginCapability::no_sort(pattern_id);

            const InfillOverrideCallStyle style =
                resolve_style
                    ? infill_cached_override_call_style(m_no_sort_style, override, inspect, 1, 0)
                    : static_cast<InfillOverrideCallStyle>(m_no_sort_style.load(std::memory_order_acquire));
            pybind11::object returned =
                style == InfillOverrideCallStyle::WithPatternId ? override(pybind11::cast(pattern_id)) : override();
            return returned.cast<bool>();
        });
    }

    bool aligned_to_origin(const std::string& pattern_id) const override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                          python_gil;
        if (!python_gil)
            throw std::runtime_error("Python interpreter is shutting down");
        const bool      resolve_style = m_aligned_to_origin_style.load(std::memory_order_acquire) < 0;
        pybind11::module_ inspect;
        if (resolve_style)
            inspect = pybind11::module_::import("inspect");
        ORCA_PY_AUDIT_SCOPE();

        ORCA_PY_LOGGED_OVERRIDE_BODY({
            pybind11::function override =
                pybind11::get_override(static_cast<const InfillPluginCapability*>(this), "aligned_to_origin");
            if (!override)
                return InfillPluginCapability::aligned_to_origin(pattern_id);

            const InfillOverrideCallStyle style =
                resolve_style
                    ? infill_cached_override_call_style(m_aligned_to_origin_style, override, inspect, 1, 0)
                    : static_cast<InfillOverrideCallStyle>(
                          m_aligned_to_origin_style.load(std::memory_order_acquire));
            pybind11::object returned =
                style == InfillOverrideCallStyle::WithPatternId ? override(pybind11::cast(pattern_id)) : override();
            return returned.cast<bool>();
        });
    }

    // Hand-rolled rather than PYBIND11_OVERRIDE: generate() returns an unregistered C++ container
    // (Polylines) and accepts an unregistered value type that must cross as an owned Python copy,
    // so the generic macro's pybind11::cast<ret> path cannot be used. Everything else is the audited
    // trampoline contract: capability ref counter, runtime lease + GIL, filesystem audit scope, and
    // traceback-preserving rethrow. The pre-multi-pattern generate(self, request) form is accepted
    // alongside the current generate(self, pattern_id, request).
    Polylines generate(const std::string& pattern_id, const InfillPluginRequest& request) override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                          python_gil;
        if (!python_gil)
            throw std::runtime_error("Python interpreter is shutting down");
        const bool      resolve_style = m_generate_style.load(std::memory_order_acquire) < 0;
        pybind11::module_ inspect;
        if (resolve_style)
            inspect = pybind11::module_::import("inspect");
        ORCA_PY_AUDIT_SCOPE();

        ORCA_PY_LOGGED_OVERRIDE_BODY({
            pybind11::function override =
                pybind11::get_override(static_cast<const InfillPluginCapability*>(this), "generate");
            if (!override)
                throw pybind11::type_error("InfillPluginCapability.generate() is not implemented");

            const InfillOverrideCallStyle style =
                resolve_style
                    ? infill_cached_override_call_style(m_generate_style, override, inspect, 2, 1)
                    : static_cast<InfillOverrideCallStyle>(m_generate_style.load(std::memory_order_acquire));

            // The plugin receives an owned snapshot; nothing in it points back into the Print graph.
            pybind11::object returned =
                style == InfillOverrideCallStyle::WithPatternId
                    ? override(pybind11::cast(pattern_id), pybind11::cast(InfillPluginRequest(request)))
                    : override(pybind11::cast(InfillPluginRequest(request)));
            return infill_paths_from_python(returned);
        });
    }

private:
    // -1 means "not measured yet". All accesses happen with the GIL held; the atomics make the
    // publication of the measured style explicit for concurrent slicing workers.
    mutable std::atomic<int> m_no_sort_style{-1};
    mutable std::atomic<int> m_aligned_to_origin_style{-1};
    mutable std::atomic<int> m_generate_style{-1};
};

} // namespace Slic3r
