#ifndef slic3r_ScriptPluginCapabilityTrampoline_hpp_
#define slic3r_ScriptPluginCapabilityTrampoline_hpp_

#include "ScriptPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"

namespace Slic3r {
class PyScriptPluginCapabilityTrampoline : public PyPluginCommonTrampoline<ScriptPluginCapability>
{
public:
    using PyPluginCommonTrampoline<ScriptPluginCapability>::PyPluginCommonTrampoline;

    ExecutionResult execute() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [] {},
            PYBIND11_OVERRIDE_PURE,
            ExecutionResult,
            ScriptPluginCapability,
            execute);
    }
};
} // namespace Slic3r

#endif
