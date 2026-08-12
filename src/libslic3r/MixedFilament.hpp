// Minimal Milestone-3 pair-mix: Virtual Mixed Filament → whole-layer A/B cadence.
// Serialization grammar (rows separated by ';'):
//   "A,B,enabled,ratio_a,ratio_b"
// Example 1:1 mix of physical 1 and 2: "1,2,1,1,1"
// Empty string = no mixes. Virtual IDs start at num_physical+1 for enabled rows in order.

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
    // cycle = max(1, ratio_a + ratio_b); ratios clamped to >=0; both 0 → A
    // pos = ((layer_index % cycle) + cycle) % cycle
    // return (pos < ratio_a) ? component_a : component_b
    // Components clamped into [1, num_physical] when resolving if out of range.
    unsigned int resolve(unsigned int filament_id_1based, size_t num_physical, int layer_index) const;

    // Expand a 1-based filament ID into 0-based physical component indices.
    // Physical IDs append themselves; mixed IDs append both components.
    void append_physical_0based(unsigned int filament_id_1based, size_t num_physical, std::vector<unsigned int> &out) const;

    const std::vector<MixedFilament> &mixed_filaments() const { return m_mixed; }

private:
    int mixed_index_from_filament_id(unsigned int filament_id_1based, size_t num_physical) const;

    std::vector<MixedFilament> m_mixed; // enabled rows only for M3
};

} // namespace Slic3r
