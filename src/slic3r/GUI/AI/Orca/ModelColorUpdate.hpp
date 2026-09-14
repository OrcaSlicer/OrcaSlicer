#pragma once
#include "libslic3r/Model.hpp"
#include <algorithm>

namespace Slic3r::GUI {
// 3MF stores a portable source basename. Only accept our UUID or GLB hash names;
// the complete mesh is still compared before transferring any paint.
inline bool same_generated_artifact_name(const std::string& saved, const std::string& incoming)
{
    auto basename = [](const std::string& value) { return value.substr(value.find_last_of("/\\") + 1); };
    const auto name = basename(incoming);
    const std::string glb_prefix = "orcaslicer-ai-glb-";
    if (name.rfind(glb_prefix, 0) == 0) {
        if (name.size() != glb_prefix.size() + 64 + 4 ||
            name.substr(name.size() - 4) != ".obj" || basename(saved) != name) return false;
        return std::all_of(name.begin() + glb_prefix.size(), name.end() - 4, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }
    const std::string prefix = name.rfind("orcaslicer-ai-finish-", 0) == 0 ? "orcaslicer-ai-finish-" : "orcaslicer-ai-";
    if (name.rfind(prefix, 0) != 0 || name.size() != prefix.size() + 40 ||
        name.substr(name.size() - 4) != ".obj" || basename(saved) != name) return false;
    for (size_t i = 0; i < 36; ++i) {
        const char c = name[prefix.size() + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Face annotations are indexed by topology. Reject a changed mesh before any
// assignment; preserve object identity, transformations and non-color settings.
inline bool update_compatible_model_colors(ModelObject& target, const ModelObject& source)
{
    if (target.volumes.size() != 1 || source.volumes.size() != 1 ||
        !target.volumes.front()->is_model_part() || !source.volumes.front()->is_model_part()) return false;
    auto& destination = *target.volumes.front();
    const auto& incoming = *source.volumes.front();
    const auto& a = destination.mesh().its;
    const auto& b = incoming.mesh().its;
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size() ||
        !std::equal(a.vertices.begin(), a.vertices.end(), b.vertices.begin(), [](const auto& x, const auto& y) { return (x.array() == y.array()).all(); }) ||
        !std::equal(a.indices.begin(), a.indices.end(), b.indices.begin(), [](const auto& x, const auto& y) { return (x.array() == y.array()).all(); })) return false;
    destination.mmu_segmentation_facets.assign(incoming.mmu_segmentation_facets);
    destination.config.set("extruder", incoming.config.extruder());
    target.config.set("extruder", source.config.extruder());
    return true;
}
} // namespace Slic3r::GUI
