#pragma once

#include "Color.hpp"
#include "TriangleSelector.hpp"

#include <cstddef>
#include <string>
#include <utility>
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

// Named Map snapshot time after take_snapshot.
// Dual-emplace (UndoRedo.cpp ~954–961): named at m_current_time, then
// unnamed topmost at ++m_current_time (= ts_after). Named is always ts_after - 1
// after a successful Map snapshot — independent of how far ts_before lags
// (undo may capture uncaptured topmost ~1096 and not rewind current ~1103).
// Linear: named == ts_before == ts_after - 1.
// After Undo: named == ts_after - 1 != ts_before.
// ts_after <= ts_before: snapshot no-op / underflow guard; return ts_before
// (product Map write never calls the helper on this path — Catch2 only).
inline size_t spectrum_map_undo_named_time(size_t ts_before, size_t ts_after)
{
    if (ts_after <= ts_before)
        return ts_before;
    return ts_after - 1;
}

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

// Sibling of SpectrumMapUndoRecord for Mixed Filaments dialog apply.
// Dialog owns mix defs + process bools; Map still owns spectrum_paint_mapped.
struct SpectrumMixDialogUndoKeys
{
    std::string mixed_filament_definitions;
    bool        enable_prime_tower               = false;
    bool        dithering_local_z_mode           = false;
    bool        dithering_local_z_whole_objects  = false;
    bool        mixed_filament_gradient_mode     = false;
};

struct SpectrumMixDialogUndoRecord
{
    bool                       active              = false;
    size_t                     snapshot_timestamp  = 0;
    SpectrumMixDialogUndoKeys  pre;
    SpectrumMixDialogUndoKeys  post;
};

inline bool spectrum_mix_dialog_undo_is_post(const SpectrumMixDialogUndoRecord &record, size_t target_timestamp)
{
    return target_timestamp > record.snapshot_timestamp;
}

inline const SpectrumMixDialogUndoKeys &spectrum_mix_dialog_undo_pick(const SpectrumMixDialogUndoRecord &record,
                                                                     size_t                            target_timestamp)
{
    return spectrum_mix_dialog_undo_is_post(record, target_timestamp) ? record.post : record.pre;
}

inline void spectrum_mix_dialog_undo_drop_if_rewritten(SpectrumMixDialogUndoRecord &record,
                                                       size_t                       active_time_before_new_snapshot)
{
    if (record.active && active_time_before_new_snapshot <= record.snapshot_timestamp)
        record.active = false;
}

// Compose mix defs when Map and/or dialog side-records are active.
// Among active writers sorted by named_time ascending: latest post with named_time < T;
// else earliest pre; else "" (both inactive — caller must not write).
// Active + "" still writes (clears). Two history orders (Map-then-Dialog / Dialog-then-Map).
inline std::string spectrum_mix_defs_at(const SpectrumMapUndoRecord       &map,
                                        const SpectrumMixDialogUndoRecord &dialog,
                                        size_t                             T)
{
    struct Candidate {
        size_t      named_time = 0;
        const std::string *pre = nullptr;
        const std::string *post = nullptr;
    };
    Candidate candidates[2];
    size_t    n = 0;
    if (map.active) {
        candidates[n].named_time = map.map_snapshot_timestamp;
        candidates[n].pre        = &map.pre.mixed_filament_definitions;
        candidates[n].post       = &map.post.mixed_filament_definitions;
        ++n;
    }
    if (dialog.active) {
        candidates[n].named_time = dialog.snapshot_timestamp;
        candidates[n].pre        = &dialog.pre.mixed_filament_definitions;
        candidates[n].post       = &dialog.post.mixed_filament_definitions;
        ++n;
    }
    if (n == 2 && candidates[1].named_time < candidates[0].named_time)
        std::swap(candidates[0], candidates[1]);

    const std::string *latest_post = nullptr;
    for (size_t i = 0; i < n; ++i) {
        if (T > candidates[i].named_time)
            latest_post = candidates[i].post;
    }
    if (latest_post != nullptr)
        return *latest_post;
    if (n > 0)
        return *candidates[0].pre;
    return {};
}

// Mix defs on project + print; four process bools on print only. Empty mix string clears.
// Returns true if any value changed. Does not touch spectrum_paint_mapped.
bool apply_spectrum_mix_dialog_keys(DynamicPrintConfig                 &project_config,
                                    DynamicPrintConfig                 &print_config,
                                    const SpectrumMixDialogUndoKeys    &keys);

// Mapped-only project write for Map+dialog compose path.
bool apply_spectrum_paint_mapped(DynamicPrintConfig &project_config, bool mapped);

} // namespace Slic3r
