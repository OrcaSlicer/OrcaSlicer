#pragma once
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
class PresetBundle;

// Strip a trailing "#N" variant suffix ("retraction_length#2" -> "retraction_length").
std::string publish_base_key(const std::string &key);

// Structural keys that are never applied onto the receiver's presets when loading a published
// 3MF (single source of truth for the denylist): applying them would rewrite the user's preset
// inheritance/structure. filament_ids is nevertheless exported via the identity list in
// filter_published_config because 3MF validation needs it - exported, never applied.
const std::set<std::string>& publish_structural_keys();

// The mixed-color filament project keys (parallel per-slot arrays, see PresetBundle's
// s_project_options): a mixed slot's full definition - which slots it blends, the sublayer
// ratios and the optional Z-gradient description. A published mixed slot always serializes
// these keys; on import they are applied into the receiver's project_config (not a filament
// preset), so the mix survives the round-trip.
const std::set<std::string>& publish_mixed_keys();

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
// The identity fields drive the created copy's naming and grouping on Full entries, the
// notification labels, and the partial type gate (publish_type) is the author's explicit
// opt-in for requiring a material type.
struct PublishedMaterialEntry {
    std::string filament_type;   // material family, e.g. "PLA" (may be empty)
    std::string filament_vendor; // e.g. "Generic", "Bambu" (may be empty)
    std::string filament_id;     // stable material id, e.g. "GFL99" (may be empty)
    // Unique preset id of the author's slot preset (e.g. Orca Filament Library "setting_id").
    // Not matched against the receiver's library; carried so identical Full entries within one
    // load share one created instance (within-load dedup key).
    std::string setting_id;
    // Canonical name of the author's slot preset (e.g. "Generic PLA @System"). On Full import
    // it names the created copy after its "@variant" tail is stripped; never matched against
    // the receiver's library.
    std::string preset_name;
    // 0-based author filament slot; -1 (hand-crafted files) is skipped.
    int         slot{-1};
    std::vector<std::string> keys;
    // "Full Publish": the whole filament preset (full_keys) is published. On the receiver
    // Full Publish always creates a standalone parentless copy (libslic3r's "Detach from
    // parent"): a new project-embedded preset ("Preset Inside Project") with the full
    // resolved config, universally compatible (compatible_printers/condition cleared).
    // It lives inside the loaded project only - never written to the user's library,
    // no existing preset is ever selected-by-reference or mutated. Identical Full
    // entries inside one load share one created instance (within-load dedup).
    bool full{false};
    // All non-structural filament keys of the author's slot preset; values travel in the file
    // config, masked to the author's slot index.
    std::vector<std::string> full_keys;
    // Vendor-agnostic (MaterialType) filament type the author requires for this slot; on a
    // partial entry's mismatch the slot is replaced with a same-type filament from the
    // receiver's library. Full entries consult no gate: they detach unconditionally, and the
    // copy carries whatever values the payload bakes.
    bool publish_type{false};
    std::string publish_type_value;
    // Required filament colour, applied on load regardless of the type match.
    bool publish_color{false};
    std::string color;
};

// "PLA High Speed" -> "PLA" (strip a space modifier); dash types like "PA-CF" are kept intact.
std::string normalize_filament_type(const std::string& type);

class DynamicPrintConfig;
// Clear the compatibility lists/conditions on a filament config so it is compatible
// with every printer and every print profile. A detached published material is
// universally compatible by construction: the baseline clone may carry machine-specific
// restrictions. Empty lists + empty conditions => compatible with everything
// (see is_compatible_with_printer, Preset.cpp:840).
void make_publish_universal(DynamicPrintConfig &config);

// Naming base for a detached published-material copy: "Generic PLA @System" ->
// "Generic PLA" (truncate at the first '@' variant tail, right-trimmed). Unchanged
// when the name carries no '@'. Empty result means "fall back to identity fields".
std::string publish_material_base_name(const std::string &preset_name);

// Minimal DynamicPrintConfig for a published 3MF export: only the selected published keys,
// material keys, identity fields and plate geometry keys.
DynamicPrintConfig filter_published_config(
    const DynamicPrintConfig &full_config,
    const std::vector<std::string> &published_keys,
    const std::vector<PublishedMaterialEntry> &material_keys);
}
