#pragma once
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
class PresetBundle;

// Structural / non-publishable setting keys, shared by the Publish dialog and the published-3MF
// overlay path in PresetBundle::load_config_file_config. These keys must never be published
// because they would rewrite the user's preset inheritance/structure. This is the single
// source of truth for the denylist.
const std::set<std::string>& publish_structural_keys();

// One option row of the printer tab's "Retraction" / "Z-Hop" optgroups (TabPrinter::build_fff,
// Tab.cpp). Key and icon id are kept together so the tab can later be migrated onto these
// lists; publishable_printer_keys() is their union, and the published-3MF loader/dialog must
// never accept printer keys outside it.
struct PublishablePrinterOption {
    const char *key;  // config key, e.g. "retraction_length"
    const char *icon; // tab icon id, e.g. "printer_extruder_retraction#length"
};

// The printer tab's "Retraction" optgroup options, in tab order.
const std::vector<PublishablePrinterOption>& publishable_printer_retraction_options();
// The printer tab's "Z-Hop" optgroup options, in tab order.
const std::vector<PublishablePrinterOption>& publishable_printer_z_hop_options();

// Printer-class retraction / z-hop keys that are publishable: the union of
// publishable_printer_retraction_options() and publishable_printer_z_hop_options(). The
// published-3MF overlay applies printer keys only when their base key is in this allowlist;
// any other printer-class key in a published file is contract-excluded (never applied, never
// reported as skipped).
const std::set<std::string>& publishable_printer_keys();

// Returns the union of setting keys that differ from the base/system preset across the current
// print, printer and filament presets (feeds the Publish dialog's pre-check).
std::vector<std::string> collect_dirty_settings_keys(const PresetBundle& bundle);

// A material-qualified set of published setting keys, chosen by the author for one of the
// materials used in the project. The identity fields let the receiver apply the keys only
// when a matching material is selected: filament_id is the most precise (stable across
// machines/vendors when present, empty for user presets); filament_type + filament_vendor
// are the fallback. Keys are base keys (no "#N" variant suffix).
struct PublishedMaterialEntry {
    std::string filament_type;   // material family, e.g. "PLA" (may be empty)
    std::string filament_vendor; // e.g. "Generic", "Bambu" (may be empty)
    std::string filament_id;     // stable material id, e.g. "GFL99" (may be empty)
    // 0-based author filament slot this entry's values came from; -1 = legacy/unspecified
    // (files written before the slot field). Slotted entries apply to the receiver's Nth
    // matching preset (N = the slot's ordinal among the author's matching slots); legacy
    // entries apply to every matching receiver preset.
    int         slot{-1};
    std::vector<std::string> keys;
    // "Full Publish": the entire filament preset of this slot is serialized (see full_keys),
    // not just the individually selected keys. On load the type gate (publish_type_value)
    // decides whether the receiver keeps its material (type match) or is replaced; a full
    // entry carries no partial keys.
    bool full{false};
    // All non-structural filament keys of the author's slot preset, present when full is true.
    // Values travel in the file config, masked to the author's slot index.
    std::vector<std::string> full_keys;
    // Vendor-agnostic, curated (MaterialType) filament type the author requires for this slot.
    // On load the receiver's slot material is matched against it; on mismatch the slot is
    // replaced with a same-type filament from the receiver's library.
    bool publish_type{false};
    std::string publish_type_value;
    // Required filament colour for this slot, applied on load regardless of the type match.
    bool publish_color{false};
    std::string color;
};

// Normalizes a filament type string against the curated MaterialType list: an exact match
// wins, then the value is stripped after its first space ("PLA High Speed" -> "PLA"); a
// value still not recognized is returned unchanged. Shared by the Publish dialog's type row
// default and by the published-3MF loader's type matching.
std::string normalize_filament_type(const std::string& type);

// Constructs a minimal DynamicPrintConfig for a published 3MF export containing only the
// author-selected published keys, material keys, material identity fields, and plate geometry keys.
class DynamicPrintConfig;
DynamicPrintConfig filter_published_config(
    const DynamicPrintConfig &full_config,
    const std::vector<std::string> &published_keys,
    const std::vector<PublishedMaterialEntry> &material_keys);
}
