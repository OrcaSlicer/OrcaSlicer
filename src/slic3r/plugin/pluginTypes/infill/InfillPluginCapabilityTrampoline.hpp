#pragma once

#include "InfillPluginCapability.hpp"

#include "slic3r/plugin/PyPluginTrampoline.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <stdexcept>
#include <string>

namespace Slic3r {

class PyInfillPluginCapabilityTrampoline : public PyPluginCommonTrampoline<InfillPluginCapability>
{
public:
    using PyPluginCommonTrampoline<InfillPluginCapability>::PyPluginCommonTrampoline;

    // Optional Fill semantics. PYBIND11_OVERRIDE (not _PURE) keeps the C++ false default when the
    // Python class does not define the method.
    bool no_sort() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE,
            bool,
            InfillPluginCapability,
            no_sort);
    }

    bool aligned_to_origin() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE,
            bool,
            InfillPluginCapability,
            aligned_to_origin);
    }

    // Hand-rolled rather than PYBIND11_OVERRIDE: generate() returns an unregistered C++ container
    // (Polylines) and accepts an unregistered value type that must cross as an owned Python copy,
    // so the generic macro's pybind11::cast<ret> path cannot be used. Everything else is the audited
    // trampoline contract: capability ref counter, runtime lease + GIL, filesystem audit scope, and
    // traceback-preserving rethrow.
    Polylines generate(const InfillPluginRequest& request) override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                          python_gil;
        if (!python_gil)
            throw std::runtime_error("Python interpreter is shutting down");
        ORCA_PY_AUDIT_SCOPE();

        try {
            pybind11::function override =
                pybind11::get_override(static_cast<const InfillPluginCapability*>(this), "generate");
            if (!override)
                throw pybind11::type_error("InfillPluginCapability.generate() is not implemented");

            // The plugin receives an owned snapshot; nothing in it points back into the Print graph.
            pybind11::object returned = override(pybind11::cast(InfillPluginRequest(request)));
            return infill_paths_from_python(returned);
        } catch (pybind11::error_already_set& err) {
            const bool audit_denial = PluginAuditManager::instance().audit_denial_pending();
            if (audit_denial)
                PluginAuditManager::instance().clear_current_plugin();
            log_python_exception_keep(err);
            if (audit_denial)
                PluginAuditManager::instance().clear_audit_denial();
            throw;
        }
    }
};

} // namespace Slic3r
