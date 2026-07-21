#pragma once
#include "SlicingPipelinePluginCapability.hpp"
#include "slic3r/plugin/PyPluginTrampoline.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include <boost/filesystem.hpp>

namespace Slic3r {
class PySlicingPipelinePluginCapabilityTrampoline : public PyPluginCommonTrampoline<SlicingPipelinePluginCapability> {
public:
    using PyPluginCommonTrampoline<SlicingPipelinePluginCapability>::PyPluginCommonTrampoline;
    ExecutionResult execute(SlicingPipelineContext& ctx) override {
        ORCA_PY_OVERRIDE_AUDITED(
            ::Slic3r::PluginAuditManager::AuditMode::Loading,
            [&]{
                // At Step.psGCodePostProcess the plugin edits the exported G-code file, which lives
                // outside data_dir() (a temp/output folder), so writing to it would otherwise be
                // blocked by the audit sandbox. Grant that folder as a scoped allowed root. The setup
                // callback runs AFTER the audit context is constructed, so the scoped root is not
                // cleared by its constructor. Empty at every other step, so no extra access is
                // granted to the geometry hooks.
                if (!ctx.gcode_path.empty())
                    ::Slic3r::PluginAuditManager::instance().add_scoped_allowed_root(
                        boost::filesystem::path(ctx.gcode_path).parent_path());
            },
            PYBIND11_OVERRIDE_PURE,
            ExecutionResult, SlicingPipelinePluginCapability, execute, ctx);
    }
};
} // namespace Slic3r
