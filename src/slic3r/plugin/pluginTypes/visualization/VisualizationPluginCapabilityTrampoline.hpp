#pragma once

#include "VisualizationPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"

namespace Slic3r {

// Loading mode matches the current capability policy and permits a visualizer to read a negotiated
// resource and launch packaged helpers. The audit hook still restricts writes to Orca-approved
// roots and blocks denied application secrets.
class PyVisualizationPluginCapabilityTrampoline : public PyPluginCommonTrampoline<VisualizationPluginCapability>
{
public:
    using PyPluginCommonTrampoline<VisualizationPluginCapability>::PyPluginCommonTrampoline;

    std::vector<VisualizationInputSpec> get_supported_inputs() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [] {},
            PYBIND11_OVERRIDE_PURE,
            std::vector<VisualizationInputSpec>,
            VisualizationPluginCapability,
            get_supported_inputs);
    }

    ExecutionResult open(const VisualizationContext& ctx) override
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

    ExecutionResult update(const VisualizationContext& ctx) override
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
