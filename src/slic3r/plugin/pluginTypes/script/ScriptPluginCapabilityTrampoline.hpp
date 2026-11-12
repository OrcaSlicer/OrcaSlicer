#ifndef slic3r_ScriptPluginCapabilityTrampoline_hpp_
#define slic3r_ScriptPluginCapabilityTrampoline_hpp_

#include "ScriptPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include <pybind11/pybind11.h>

namespace Slic3r {
class PyScriptPluginCapabilityTrampoline : public PyPluginCommonTrampoline<ScriptPluginCapability>
{
public:
    using PyPluginCommonTrampoline<ScriptPluginCapability>::PyPluginCommonTrampoline;

    ExecutionResult execute() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE_PURE,
            ExecutionResult,
            ScriptPluginCapability,
            execute);
    }
};
} // namespace Slic3r

#endif
