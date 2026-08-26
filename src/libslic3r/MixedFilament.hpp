// Milestone-3/4/5 pair-mix + 0009 3-component: virtual mixed filament → whole-layer cadence or pattern.
// Serialization grammar (rows separated by ';'):
//   "A,B,enabled,ratio_a,ratio_b[,pattern][,xaN][,xbN][,cN][,rcN]"
// Pair prefix is load-bearing. Example 1:1 mix of physical 1 and 2: "1,2,1,1,1"
// Example pattern: "1,2,1,1,1,112" → layers T0,T0,T1 when A=1,B=2
// Optional cN/rcN: 1,2,1,1,1,c3,rc1 → period ra+rb+rc, A then B then C.
// Optional trailing xa/xb tokens persist component surface offsets (mm).
// Empty string = no mixes. Virtual IDs start at num_physical+1 for enabled rows in order.
// Token map (1-based): '1'→component_a, '2'→component_b,
//   '3'→component_c when set else physical 3, '4'..'9'→direct physical ID.

#pragma once

#include "ExPolygon.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

struct MixedFilament
{
    unsigned int component_a = 1; // 1-based physical
    unsigned int component_b = 2;
    unsigned int component_c = 0; // 0 = unset pair-only
    int          ratio_a     = 1;
    int          ratio_b     = 1;
    int          ratio_c     = 0; // 0 = unset; default 1 when cN is present without rcN
    bool         enabled     = true;
    // Optional whole-layer cycle pattern. Empty → ratio cadence.
    std::string  manual_pattern;
    // XY surface offsets (mm). Positive contracts inward; negative expands.
    // Applied only when Local-Z does not own the layer (FS truth).
    float        component_a_surface_offset = 0.f;
    float        component_b_surface_offset = 0.f;
};

class MixedFilamentManager
{
public:
    void        clear();
    void        load_definitions(const std::string &serialized);
    std::string serialize_definitions() const;

    size_t enabled_count() const { return m_mixed.size(); }
    // Total 1-based filament IDs available: physical + enabled mixes.
    size_t total_filaments(size_t num_physical) const { return num_physical + m_mixed.size(); }

    bool is_mixed(unsigned int filament_id_1based, size_t num_physical) const;
    // Virtual IDs: num_physical+1, num_physical+2, ... enumerate enabled rows in order.
    const MixedFilament *mixed_filament_from_id(unsigned int filament_id_1based, size_t num_physical) const;

    // Surface offset (mm) for the component that owns this mixed ID on layer_index.
    // 0 if not mixed. A → xa, B → xb, else 0 (C-layer does not inherit xb).
    float component_surface_offset(unsigned int filament_id_1based, size_t num_physical, int layer_index) const;

    // Resolve virtual → physical for a layer. Non-mixed IDs returned unchanged.
    // If manual_pattern non-empty after normalize: pos = layer_index % len; token map.
    // Else if component_c set: cycle = ra+rb+rc; pos = layer_index % cycle; A then B then C.
    // Else ratio: cycle = max(1, ratio_a + ratio_b); pos = layer_index % cycle;
    //   return (pos < ratio_a) ? component_a : component_b
    // Components clamped into [1, num_physical] when resolving if out of range.
    unsigned int resolve(unsigned int filament_id_1based, size_t num_physical, int layer_index) const;

    // Expand a 1-based filament ID into 0-based physical component indices.
    // Physical IDs append themselves; mixed IDs append unique A, B, C-if-set, and pattern tokens.
    void append_physical_0based(unsigned int filament_id_1based, size_t num_physical, std::vector<unsigned int> &out) const;

    const std::vector<MixedFilament> &mixed_filaments() const { return m_mixed; }

    // Flatten separators (/,-_|:; , space) from a pattern into compact digit tokens.
    static std::string normalize_manual_pattern(const std::string &pattern);

    // Physical + enabled mixes. Used by 3mf/GUI clamps so virtual IDs survive open.
    static size_t max_filament_id(const std::string &serialized, size_t num_physical);

private:
    int mixed_index_from_filament_id(unsigned int filament_id_1based, size_t num_physical) const;

    std::vector<MixedFilament> m_mixed; // enabled rows only
};

// Offset mixed-region slices by a component surface bias (mm).
// Positive contracts inward; negative expands. Empty in / failed offset → {}.
ExPolygons apply_surface_offset(const ExPolygons &src, float offset_mm);

// Clamp/slice/gizmo persist policy. TriangleSelector 4-bit packing can encode state 16;
// 0008 does not promise Extruder16 3mf round-trip (fold-in: cap 15).
constexpr size_t SPECTRUM_PAINT_ID_PERSIST_CAP = 15;

// Shared paint-ID clamp for update_extruder_count and the delete-filament sibling.
// min(15, max(physical_n, max_filament_id, source_palette_size)).
size_t spectrum_paint_id_limit(size_t physical_n, size_t max_filament_id, size_t source_palette_size);

// True if applying new_serialized would change the mix (A/B/C/ratio/pattern) of any painted mix ID.
// Mix IDs are enabled-row order; disabling a middle row shifts later IDs. No silent remap.
bool mixed_filament_painted_ids_would_shift(const std::string     &old_serialized,
                                            const std::string     &new_serialized,
                                            size_t                 num_physical,
                                            const std::vector<int> &painted_filament_ids);

} // namespace Slic3r
