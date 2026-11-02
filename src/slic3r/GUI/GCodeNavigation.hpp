#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <array>

#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"

namespace Slic3r::GUI {

struct GCodeNavigationTarget
{
    uint32_t line_id;
    size_t first_vertex;
    size_t last_vertex;
};

struct GCodeNavigationCategories
{
    std::array<bool, libvgcode::GCODE_EXTRUSION_ROLES_COUNT> roles{};
    std::array<bool, libvgcode::OPTION_TYPES_COUNT> options{};
};

inline void enable_gcode_navigation_categories(libvgcode::Viewer& viewer, const GCodeNavigationCategories& categories)
{
    for (size_t i = 0; i < categories.roles.size(); ++i) {
        const auto role = static_cast<libvgcode::EGCodeExtrusionRole>(i);
        if (categories.roles[i] && !viewer.is_extrusion_role_visible(role))
            viewer.toggle_extrusion_role_visibility(role);
    }
    for (size_t i = 0; i < categories.options.size(); ++i) {
        const auto option = static_cast<libvgcode::EOptionType>(i);
        if (categories.options[i] && !viewer.is_option_visible(option))
            viewer.toggle_option_visibility(option);
    }
}

template<class VertexAt>
GCodeNavigationCategories gcode_navigation_categories(const GCodeNavigationTarget& target, const VertexAt& vertex_at)
{
    GCodeNavigationCategories categories;
    for (size_t i = target.first_vertex; i <= target.last_vertex; ++i) {
        const auto& vertex = vertex_at(i);
        if (vertex.type == libvgcode::EMoveType::Extrude) {
            const size_t role = static_cast<size_t>(vertex.role);
            if (role < categories.roles.size())
                categories.roles[role] = true;
        } else {
            const size_t option = static_cast<size_t>(libvgcode::move_type_to_option(vertex.type));
            if (option < categories.options.size())
                categories.options[option] = true;
        }
    }
    return categories;
}

// The accessor reads the viewer's existing vertices without copying a second line index.
// The caller checks ordering once when loading a result; unusual order uses a linear fallback.
template<class LineIdAt>
std::optional<GCodeNavigationTarget> find_gcode_navigation_target(
    uint32_t requested_line, size_t vertex_count, const LineIdAt& line_id_at, bool sorted)
{
    if (requested_line == 0 || vertex_count == 0)
        return std::nullopt;

    size_t candidate = vertex_count;
    if (sorted) {
        auto upper_bound = [&](uint32_t line) {
            size_t first = 0, last = vertex_count;
            while (first < last) {
                const size_t mid = first + (last - first) / 2;
                if (line_id_at(mid) <= line)
                    first = mid + 1;
                else
                    last = mid;
            }
            return first;
        };
        const size_t next = upper_bound(requested_line);
        candidate = next == 0 ? 0 : next - 1;
        if (line_id_at(candidate) == 0)
            candidate = upper_bound(0);
    } else {
        uint32_t preceding_line = 0;
        uint32_t first_source_line = UINT32_MAX;
        for (size_t i = 0; i < vertex_count; ++i) {
            const uint32_t line = line_id_at(i);
            if (line == 0)
                continue;
            if (line <= requested_line && line >= preceding_line) {
                preceding_line = line;
                candidate = i;
            } else if (preceding_line == 0 && line <= first_source_line) {
                first_source_line = line;
                candidate = i;
            }
        }
    }
    if (candidate == vertex_count || line_id_at(candidate) == 0)
        return std::nullopt;

    const uint32_t line = line_id_at(candidate);
    size_t first = candidate, last = candidate;
    while (first > 0 && line_id_at(first - 1) == line)
        --first;
    while (last + 1 < vertex_count && line_id_at(last + 1) == line)
        ++last;
    return GCodeNavigationTarget{line, first, last};
}

} // namespace Slic3r::GUI
