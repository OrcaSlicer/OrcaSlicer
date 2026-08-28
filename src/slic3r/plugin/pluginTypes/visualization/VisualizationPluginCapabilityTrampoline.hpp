#pragma once

#include "VisualizationPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"

namespace Slic3r {

class PyVisualizationPluginCapabilityTrampoline : public PyPluginCommonTrampoline<VisualizationPluginCapability>
{
public:
    using PyPluginCommonTrampoline<VisualizationPluginCapability>::PyPluginCommonTrampoline;

    ExecutionResult open(VisualizationContext& ctx) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [] {},
            PYBIND11_OVERRIDE_PURE,
            ExecutionResult,
            VisualizationPluginCapability,
            open,
            ctx);
    }

    ExecutionResult update(VisualizationContext& ctx) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [] {},
            PYBIND11_OVERRIDE_PURE,
            ExecutionResult,
            VisualizationPluginCapability,
            update,
            ctx);
    }

    void close() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [] {},
            PYBIND11_OVERRIDE,
            void,
            VisualizationPluginCapability,
            close);
    }
};

} // namespace Slic3r
