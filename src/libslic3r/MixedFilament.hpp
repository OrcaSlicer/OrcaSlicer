// Milestone-3/4 pair-mix: virtual mixed filament → whole-layer A/B cadence or pattern.
// Serialization grammar (rows separated by ';'):
//   "A,B,enabled,ratio_a,ratio_b[,pattern]"
// Example 1:1 mix of physical 1 and 2: "1,2,1,1,1"
// Example pattern: "1,2,1,1,1,112" → layers T0,T0,T1 when A=1,B=2
// Empty string = no mixes. Virtual IDs start at num_physical+1 for enabled rows in order.
// Token map (1-based): '1'→component_a, '2'→component_b, '3'..'9'→direct physical ID.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

struct MixedFilament
{
    unsigned int component_a = 1; // 1-based physical
    unsigned int component_b = 2;
    int          ratio_a     = 1;
    int          ratio_b     = 1;
    bool         enabled     = true;
    // Optional whole-layer cycle pattern. Empty → ratio cadence.
    std::string  manual_pattern;
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

    // Resolve virtual → physical for a layer. Non-mixed IDs returned unchanged.
    // If manual_pattern non-empty after normalize: pos = layer_index % len; token map.
    // Else ratio: cycle = max(1, ratio_a + ratio_b); pos = layer_index % cycle;
    //   return (pos < ratio_a) ? component_a : component_b
    // Components clamped into [1, num_physical] when resolving if out of range.
    unsigned int resolve(unsigned int filament_id_1based, size_t num_physical, int layer_index) const;

    // Expand a 1-based filament ID into 0-based physical component indices.
    // Physical IDs append themselves; mixed IDs append both components.
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

} // namespace Slic3r
