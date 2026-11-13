#pragma once
#include "FirstLayerFlowMath.hpp"
#include "FirstLayerFlowMeshChecks.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace flf {
// Immutable plan; no Orca model or presets are modified.
struct LabelPlan {
    int patch_number;
    std::string text;
    double multiplier;
    Mesh geometry; // local-space geometry; placement is caller's responsibility
};
inline std::array<LabelPlan,11> make_label_plan(double filament_flow) {
    const auto patches=FirstLayerFlow::calculate(filament_flow);
    std::array<LabelPlan,11> result{};
    for(size_t i=0;i<result.size();++i) {
        char buf[32];
        if(std::snprintf(buf,sizeof(buf),"%.2f",patches[i].displayed_target)<0)
            throw std::runtime_error("label formatting failed");
        auto geometry=label_mesh(buf);
        validate_single_layer(geometry);
        result[i]={patches[i].number,buf,patches[i].multiplier,std::move(geometry)};
    }
    return result;
}
} // namespace flf
