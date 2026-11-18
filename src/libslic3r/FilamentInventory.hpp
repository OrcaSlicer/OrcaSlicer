#ifndef slic3r_FilamentInventory_hpp_
#define slic3r_FilamentInventory_hpp_

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

// What one reported printer slot (an extruder, a lane, a gate) currently holds, and where it sits.
struct PhysicalFilament
{
    int         id   = 0;        // stable per-machine id, > 0; 0 = unassigned
    std::string color;           // "#RRGGBB"; empty = unknown
    std::string type;            // empty = unknown
    std::string preset;          // exact filament preset name; empty = unknown
    std::string name;            // the printer's own slot name ("lane1", "T3")
    std::string unit;            // stable id of the changer unit ("Turtle_1"); empty = flat / unknown
    std::string unit_label;      // the unit's display name
    std::string head;            // display name of the extruder it feeds; empty = unknown
    int         slot = 0;        // position within its unit, 0-based
    int         extruder = -1;   // 0-based physical extruder it feeds; -1 = unknown
    int         virtual_tool = -1; // the T<n> the printer currently maps this slot to; -1 = unknown
    bool empty() const { return color.empty() && type.empty() && preset.empty(); }
};

// The physical slots a printer reported, in the agent's order. Before the first sync there is one
// slot per nozzle.
struct FilamentInventory
{
    std::vector<PhysicalFilament> slots;
    int next_id = 1;                                   // persisted id allocator
    std::string dialect;                               // changer dialect the last sync read ("afc", "happy_hare", "")

    std::string serialize() const;
    // Malformed or short input yields fallback_slot_count empty slots. A legacy per-tool record
    // flattens to its loaded rows; swap rows are dropped.
    static FilamentInventory deserialize(const std::string& s, size_t fallback_slot_count);

    const PhysicalFilament* find(int id) const; // nullptr if absent

    // Mints ids for non-empty slots lacking one and clears ids on empty slots. Call before saving:
    // a non-empty slot saved with id 0 is renumbered on the next load, breaking plate maps that
    // reference it.
    void ensure_ids();

    // Pads up to n slots; never drops any.
    void ensure_slot_count(size_t n);
};

// Inventories keyed by printer preset name, since connection settings are per preset too.
// serialize() writes {"version": 1, "presets": {...}}; deserialize() also reads the legacy bare
// map. An object is read as the envelope only when it has an integer "version" and an object
// "presets".
struct FilamentInventories
{
    std::map<std::string, FilamentInventory> by_preset;
    // Set when the input was invalid JSON or not an object; by_preset is then empty. Callers that
    // save back to the same key must back up the original first or the stored data is lost.
    bool parse_error = false;

    std::string                serialize() const;
    static FilamentInventories deserialize(const std::string& s); // malformed -> empty store, parse_error = true

    // Creates an empty inventory on first use and pads it to at least slot_count slots.
    FilamentInventory& for_preset(const std::string& printer_preset_name, size_t slot_count);
};

// Strips the vendor and type tokens (case-insensitive, whole tokens) from a filament preset name:
// "Snapmaker PLA SnapSpeed" -> "SnapSpeed"; "Generic PLA" -> "".
std::string derive_filament_subtype(const std::string& display_name, const std::string& vendor, const std::string& type);

// True when every slot is empty(); the mapping dialog then picks bare slots instead of filaments.
bool inventory_all_unset(const FilamentInventory& inv);

// Exact match (case/whitespace-insensitive) or same family (PLA/PLA+, PETG/PETG+). Empty on either
// side is not compatible; callers handle unknown types.
bool type_compatible(const std::string& project_type, const std::string& slot_type);

// The mapping dialog's proposal, one entry per plate_filaments entry: the physical filament id
// auto_map_filaments picks for it, 0 when none.
std::vector<int> compute_physical_map_proposal(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_vendors,
    const std::vector<std::string>& filament_presets,
    const std::vector<int>&         plate_filaments,
    const FilamentInventory&        inventory);

struct ProjectFilamentInfo
{
    std::string type;   // empty = unknown
    std::string color;  // "#RRGGBB" or "#RRGGBBAA"; empty = unknown
    std::string vendor;
    std::string preset; // exact filament preset name; empty = unknown
};

// The physical id per project filament, -1 when unresolved. Resolves each by priority: (1) the
// color-nearest slot with the same preset; (2) the color-nearest type_compatible slot, tie-broken by
// vendor then slot index. Both tiers reject slots beyond kMaxFamilyColorDistance unless a color is
// unknown. Several filaments may share a slot.
std::vector<int> auto_map_filaments(const std::vector<ProjectFilamentInfo>& project, const FilamentInventory& inventory);

} // namespace Slic3r

#endif // slic3r_FilamentInventory_hpp_
