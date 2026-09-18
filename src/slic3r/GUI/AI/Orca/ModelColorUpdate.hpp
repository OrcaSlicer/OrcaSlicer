#pragma once
#include "libslic3r/Model.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include <algorithm>
#include <cmath>
#include <map>

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

// Apply semantic midpoint leaves directly to the existing MMU tree. This is
// transactional and requires the exact source topology; no remeshing or
// nearest-face recovery is permitted.
inline bool apply_subface_color_overrides(
    const indexed_triangle_set& expected, const indexed_triangle_set& actual,
    TriangleSelector::TriangleSplittingData& painting,
    const std::vector<std::pair<size_t, std::array<float, 3>>>& whole_faces,
    const std::vector<AI::ModelSubfaceColorOverride>& subfaces, std::string& error)
{
    error.clear();
    auto fail = [&error](const char* message) { error = message; return false; };
    if (subfaces.empty()) return true;
    if (expected.vertices.size() != actual.vertices.size() || expected.indices.size() != actual.indices.size() ||
        !std::equal(expected.vertices.begin(), expected.vertices.end(), actual.vertices.begin(),
            [](const auto& lhs, const auto& rhs) { return (lhs.array() == rhs.array()).all(); }) ||
        !std::equal(expected.indices.begin(), expected.indices.end(), actual.indices.begin(),
            [](const auto& lhs, const auto& rhs) { return (lhs.array() == rhs.array()).all(); }))
        return fail("The imported topology changed before semantic subfaces were applied.");

    TriangleMesh mesh(actual);
    TriangleSelector selector(mesh);
    selector.deserialize(painting);
    std::map<size_t, std::array<float, 3>> root_colors;
    for (const auto& item : whole_faces) {
        if (item.first >= actual.indices.size() || std::any_of(item.second.begin(), item.second.end(), [](float value) {
                return !std::isfinite(value) || value < 0.f || value > 1.f;
            })) return fail("A whole-face material reference is invalid.");
        root_colors[item.first] = item.second;
    }
    std::map<std::array<float, 3>, EnforcerBlockerType> color_states;
    for (const auto& item : root_colors) {
        EnforcerBlockerType state;
        if (!selector.facet_state(int(item.first), state))
            return fail("Whole-face painting was split before semantic subfaces were applied.");
        const auto found = color_states.find(item.second);
        if (found != color_states.end() && found->second != state)
            return fail("One target color maps to multiple imported material states.");
        color_states[item.second] = state;
    }

    std::map<size_t, std::vector<TriangleSelector::MidpointSubfaceState>> grouped;
    for (const AI::ModelSubfaceColorOverride& item : subfaces) {
        if (item.face_id >= actual.indices.size() || item.depth == 0 || item.depth > 2 ||
            unsigned(item.path) >= (1u << (2u * item.depth)) ||
            std::any_of(item.color.begin(), item.color.end(), [](float value) {
                return !std::isfinite(value) || value < 0.f || value > 1.f;
            })) return fail("A semantic subface material reference is invalid.");
        const auto state = color_states.find(item.color);
        if (state == color_states.end())
            return fail("A semantic subface target has no imported material state.");
        grouped[item.face_id].push_back({item.depth, item.path, state->second});
    }
    for (const auto& face : grouped) {
        EnforcerBlockerType root_state;
        if (!selector.facet_state(int(face.first), root_state) ||
            !selector.set_facet_midpoint_subfaces(int(face.first), root_state, face.second))
            return fail("The semantic subface tree is invalid for the imported face.");
    }
    painting = selector.serialize();
    return true;
}
} // namespace Slic3r::GUI
