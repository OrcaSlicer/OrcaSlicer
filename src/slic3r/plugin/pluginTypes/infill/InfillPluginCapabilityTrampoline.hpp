#pragma once

#include "InfillPluginCapability.hpp"

#include "pybind11/cast.h"
#include "slic3r/plugin/PyPluginTrampoline.hpp"

#include <libslic3r/Polyline.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

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
    // Python class does not define the method. The pattern ID is always passed, so an override that
    // declares a different arity receives the normal Python TypeError.
    bool no_sort(const std::string& pattern_id) const override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                          python_gil;
        if (!python_gil)
            throw std::runtime_error("Python interpreter is shutting down");
        ORCA_PY_AUDIT_SCOPE();

        ORCA_PY_LOGGED_OVERRIDE_BODY({
            pybind11::function override =
                pybind11::get_override(static_cast<const InfillPluginCapability*>(this), "no_sort");
            if (!override)
                return InfillPluginCapability::no_sort(pattern_id);
            return override(pybind11::cast(pattern_id)).cast<bool>();
        });
    }

    bool aligned_to_origin(const std::string& pattern_id) const override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                          python_gil;
        if (!python_gil)
            throw std::runtime_error("Python interpreter is shutting down");
        ORCA_PY_AUDIT_SCOPE();

        ORCA_PY_LOGGED_OVERRIDE_BODY({
            pybind11::function override =
                pybind11::get_override(static_cast<const InfillPluginCapability*>(this), "aligned_to_origin");
            if (!override)
                return InfillPluginCapability::aligned_to_origin(pattern_id);
            return override(pybind11::cast(pattern_id)).cast<bool>();
        });
    }

    // Hand-rolled rather than PYBIND11_OVERRIDE: generate() returns an unregistered C++ container
    // (Polylines) and accepts an unregistered value type that must cross as an owned Python copy,
    // so the generic macro's pybind11::cast<ret> path cannot be used. Everything else is the audited
    // trampoline contract: capability ref counter, runtime lease + GIL, filesystem audit scope, and
    // traceback-preserving rethrow.
    Polylines generate(const std::string& pattern_id, const InfillPluginRequest& request) override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                          python_gil;
        if (!python_gil)
            throw std::runtime_error("Python interpreter is shutting down");
        ORCA_PY_AUDIT_SCOPE();

        ORCA_PY_LOGGED_OVERRIDE_BODY({
            pybind11::function override =
                pybind11::get_override(static_cast<const InfillPluginCapability*>(this), "generate");
            if (!override)
                throw pybind11::type_error("InfillPluginCapability.generate() is not implemented");

            // The plugin receives an owned snapshot; nothing in it points back into the Print graph.
            pybind11::object returned =
                override(pybind11::cast(pattern_id), pybind11::cast(InfillPluginRequest(request)));
            return infill_paths_from_python(returned);
        });
    }
};

} // namespace Slic3r
