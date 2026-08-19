#pragma once
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
class PresetBundle;

// Structural keys that must never be published (single source of truth for the denylist):
// publishing them would rewrite the user's preset inheritance/structure.
const std::set<std::string>& publish_structural_keys();

// One row of the printer tab's "Retraction" / "Z-Hop" optgroups (key + tab icon id), kept
// together so the tab can later be migrated onto these lists.
struct PublishablePrinterOption {
    const char *key;  // config key, e.g. "retraction_length"
    const char *icon; // tab icon id, e.g. "printer_extruder_retraction#length"
};

// The printer tab's "Retraction" / "Z-Hop" optgroup options, in tab order.
const std::vector<PublishablePrinterOption>& publishable_printer_retraction_options();
const std::vector<PublishablePrinterOption>& publishable_printer_z_hop_options();

// Union of the two optgroup option lists; the published-3MF overlay applies printer keys only
// when their base key is in this allowlist (anything else is contract-excluded).
const std::set<std::string>& publishable_printer_keys();

// Union of setting keys differing from the base/system preset across the current print,
// printer and filament presets (feeds the Publish dialog's pre-check).
std::vector<std::string> collect_dirty_settings_keys(const PresetBundle& bundle);

// Per-slot published material keys, applied positionally (author slot N -> receiver slot N).
// The identity fields are carried for reference/notification labels only; the type gate
// (publish_type) is the author's explicit opt-in for requiring a material type.
struct PublishedMaterialEntry {
    std::string filament_type;   // material family, e.g. "PLA" (may be empty)
    std::string filament_vendor; // e.g. "Generic", "Bambu" (may be empty)
    std::string filament_id;     // stable material id, e.g. "GFL99" (may be empty)
    // Unique preset id of the author's slot preset (e.g. Orca Filament Library "setting_id");
    // used on load to match the exact published variant, which filament_id alone cannot
    // distinguish ("Generic PLA" and "Generic PLA Matte" share their inherited id).
    std::string setting_id;
    // Canonical name of the author's slot preset (e.g. "Generic PLA @System"). The receiver
    // prefers an exact name/alias match over id matching: ids can be shared across variants
    // or missing from older files, the name is what the author actually selected.
    std::string preset_name;
    // 0-based author filament slot; -1 (hand-crafted files) is skipped.
    int         slot{-1};
    std::vector<std::string> keys;
    // "Full Publish": serialize the whole filament preset (full_keys); the type gate then
    // decides whether the receiver keeps its material (type match) or is replaced.
    bool full{false};
    // All non-structural filament keys of the author's slot preset; values travel in the file
    // config, masked to the author's slot index.
    std::vector<std::string> full_keys;
    // Vendor-agnostic (MaterialType) filament type the author requires for this slot; on
    // mismatch the slot is replaced with a same-type filament from the receiver's library.
    bool publish_type{false};
    std::string publish_type_value;
    // Required filament colour, applied on load regardless of the type match.
    bool publish_color{false};
    std::string color;
};

// "PLA High Speed" -> "PLA" (strip a space modifier); dash types like "PA-CF" are kept intact.
std::string normalize_filament_type(const std::string& type);

// Minimal DynamicPrintConfig for a published 3MF export: only the selected published keys,
// material keys, identity fields and plate geometry keys.
class DynamicPrintConfig;
DynamicPrintConfig filter_published_config(
    const DynamicPrintConfig &full_config,
    const std::vector<std::string> &published_keys,
    const std::vector<PublishedMaterialEntry> &material_keys);
}
