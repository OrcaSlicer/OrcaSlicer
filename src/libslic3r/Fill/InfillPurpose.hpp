#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace Slic3r {

// The surface category that selected an infill pattern. It is the contract between the four
// plugin-backed pattern settings, the infill registry's per-pattern metadata, and the plugin
// request, so a pattern selected for one surface never silently runs as another.
enum class InfillSurfacePurpose
{
    Sparse,
    InternalSolid,
    TopSurface,
    BottomSurface,
};

// One plugin-selectable infill pattern setting and the surface purpose it drives. The single
// explicit mapping keeps the four settings, the GUI dropdowns, the project preflight and the
// registry's supported-surface metadata from drifting apart.
struct InfillPatternSetting
{
    std::string_view     option_key;
    InfillSurfacePurpose purpose;
};

inline constexpr std::array<InfillPatternSetting, 4> infill_pattern_settings{ {
    { "sparse_infill_pattern", InfillSurfacePurpose::Sparse },
    { "internal_solid_infill_pattern", InfillSurfacePurpose::InternalSolid },
    { "top_surface_pattern", InfillSurfacePurpose::TopSurface },
    { "bottom_surface_pattern", InfillSurfacePurpose::BottomSurface },
} };

// Purpose of a plugin-selectable pattern setting, or nullopt for any other option.
inline std::optional<InfillSurfacePurpose> infill_purpose_for_option(std::string_view option_key)
{
    for (const InfillPatternSetting& setting : infill_pattern_settings)
        if (setting.option_key == option_key)
            return setting.purpose;
    return std::nullopt;
}

} // namespace Slic3r
