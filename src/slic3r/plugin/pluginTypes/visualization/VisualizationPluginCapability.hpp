#pragma once

#include "../../PythonPluginInterface.hpp"

#include <cstdint>
#include <string>

namespace pybind11 {
class module_;
}

namespace Slic3r {

struct VisualizationContext : PluginContext
{
    uint64_t    scene_id{0};
    int         plate_index{-1};
    std::string geometry_path;
    std::string metadata_json;
};

class VisualizationPluginCapability : public PluginCapabilityInterface
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::Visualization; }

    virtual ExecutionResult open(VisualizationContext& ctx)   = 0;
    virtual ExecutionResult update(VisualizationContext& ctx) = 0;
    virtual void            close() {}
};

} // namespace Slic3r
