#pragma once

#include "Color.hpp"
#include "TriangleSelector.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class ModelVolume;

struct SpectrumPaintBakePlan
{
    bool                      valid = false;
    std::string               mixed_filament_definitions; // ';' rows; empty if all Physical
    EnforcerBlockerStateMap   slot_map{};                 // identity except mapped source IDs
    size_t                    mix_count             = 0;
    size_t                    physical_mapped_count = 0;
    std::string               error;                      // if !valid
};

// mix_base = live filament slot count; Mix dest IDs = mix_base + 1…
// Match lattice still uses first min(physicals.size(), 4) colours.
SpectrumPaintBakePlan plan_spectrum_paint_bake(
    const std::vector<std::string> &source_hexes, // spectrum_source_filament_colour
    const std::vector<ColorRGB>    &physicals,
    size_t                          mix_base);

// Deserialize (keep IDs 5–8), remap_triangle_state, FacetsAnnotation::set.
bool apply_spectrum_paint_bake(ModelVolume &vol, const EnforcerBlockerStateMap &slot_map);

// Side-record keys for Map undo/redo (project + print mix defs). Not Model cereal.
struct SpectrumMapUndoKeys
{
    bool        mapped = false;
    std::string mixed_filament_definitions;
};

struct SpectrumMapUndoRecord
{
    bool                active                  = false;
    size_t              map_snapshot_timestamp  = 0;
    SpectrumMapUndoKeys pre;
    SpectrumMapUndoKeys post;
};

// Strictly >; loading the Map snapshot itself restores pre.
inline bool spectrum_map_undo_is_post(const SpectrumMapUndoRecord &record, size_t target_timestamp)
{
    return target_timestamp > record.map_snapshot_timestamp;
}

inline const SpectrumMapUndoKeys &spectrum_map_undo_pick(const SpectrumMapUndoRecord &record, size_t target_timestamp)
{
    return spectrum_map_undo_is_post(record, target_timestamp) ? record.post : record.pre;
}

// Deactivate when a new main-stack snapshot is taken at or before the named Map time
// (Undo-of-Map + new edit rewrites the Map redo branch).
inline void spectrum_map_undo_drop_if_rewritten(SpectrumMapUndoRecord &record, size_t active_time_before_new_snapshot)
{
    if (record.active && active_time_before_new_snapshot <= record.map_snapshot_timestamp)
        record.active = false;
}

// Apply mapped + mix-def onto project config; optionally print preset mix defs too.
// Returns true if any key value changed.
bool apply_spectrum_map_keys(DynamicPrintConfig             &project_config,
                             const SpectrumMapUndoKeys      &keys,
                             DynamicPrintConfig             *print_config = nullptr);

} // namespace Slic3r
