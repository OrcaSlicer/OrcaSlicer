#pragma once
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r {
class PresetBundle;
class DynamicPrintConfig;

// Strip a trailing "#N" variant suffix ("retraction_length#2" -> "retraction_length").
std::string publish_base_key(const std::string &key);

// Parse a trailing "#N" index ("retraction_length#2" -> 2). Returns -1 when the key carries no
// '#' separator or its suffix is malformed.
int publish_key_index(const std::string &key);

// Structural keys that are never applied onto the receiver's presets when loading a published
// 3MF (single source of truth for the denylist); applying them would rewrite the user's preset
// inheritance/structure. filament_ids is still exported via the identity list (3MF validation
// needs it) - exported, never applied.
const std::set<std::string>& publish_structural_keys();

// The mixed-color filament project keys (parallel per-slot arrays, see PresetBundle's
// s_project_options). Import applies them into project_config, not a filament preset.
const std::set<std::string>& publish_mixed_keys();

// One row of the printer tab's "Retraction" / "Z-Hop" optgroups (config key + tab icon id).
struct PublishablePrinterOption {
    const char *key;  // config key, e.g. "retraction_length"
    const char *icon; // tab icon id, e.g. "printer_extruder_retraction#length"
};

// The printer tab's "Retraction" / "Z-Hop" optgroup options, in tab order.
const std::vector<PublishablePrinterOption>& publishable_printer_retraction_options();
const std::vector<PublishablePrinterOption>& publishable_printer_z_hop_options();

// Union of the two optgroup option lists; printer keys apply on import only if their base
// key is in this allowlist.
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
    // "Full Publish": the whole filament preset (full_keys) is published. On the receiver Full
    // Publish always creates a standalone parentless copy (libslic3r's "Detach from parent"),
    // universally compatible and project-embedded only - never written to the user's library.
    // Identical Full entries within one load share one created instance (within-load dedup).
    bool full{false};
    // All non-structural filament keys of the author's slot preset; values travel in the file
    // config, masked to the author's slot index.
    std::vector<std::string> full_keys;
    // Vendor-agnostic (MaterialType) filament type the author requires for this slot; on a
    // partial entry's mismatch the slot is replaced with a same-type filament. Full entries
    // consult no gate.
    bool publish_type{false};
    std::string publish_type_value;
    // Required filament colour, applied on load regardless of the type match.
    bool publish_color{false};
    std::string color;
    // Import-side only, never serialized: the authored slot sits past the receiver's physical
    // capacity, so the entry is appended as an empty mixed-filament placeholder (virtual tail
    // slot; the GUI flags it for the user to assign components).
    bool mixed_placeholder{false};
};

// The author's printer_extruder_id and printer_extruder_variant, one entry per per-variant index
// of a published printer key. Ids are 1-based extruder numbers.
struct PublishedExtruderLayout
{
    std::vector<int>         extruder_ids;
    std::vector<std::string> variants;
};

// "PLA High Speed" -> "PLA" (strip a space modifier); dash types like "PA-CF" are kept intact.
std::string normalize_filament_type(const std::string& type);

// File columns of one filament slot in the uncompacted config, from filament_self_index. A
// variant-bearing key (filament_options_with_variant) stores one element per (slot, variant), and
// those elements are concatenated across slots in preset order, so the element index is a file
// column rather than a slot. A config without filament_self_index falls back to one column at
// `slot`. The overload taking the index vector serves callers whose config no longer carries it.
std::vector<size_t> publish_filament_columns(const std::vector<int> &self_index, size_t slot);
std::vector<size_t> publish_filament_columns(const DynamicPrintConfig &uncompacted, size_t slot);

// The file column of a local variant index (a position in the slot's own variant list).
std::optional<size_t> publish_filament_local_to_file(const DynamicPrintConfig &uncompacted, size_t slot, size_t local);

// Exact variant-string match. With `slot`, `config` is the uncompacted config, the search is
// limited to that slot's columns, and the result is a file column. Without `slot`, `config` is one
// preset and the result is a local index. No fallback to another variant.
std::optional<size_t> publish_match_variant(const DynamicPrintConfig &config, std::optional<size_t> slot,
                                            std::string_view variant);

// The column the slot prints with. `uncompacted` is a full config (full_config(false)): it carries
// filament_map, filament_volume_map, nozzle_volume_type, extruder_type, filament_self_index and the
// concatenated filament variants. exact is false when the printer's variant is not defined by the
// preset; file_column is then the slot's first column, which is the value the slicer prints.
struct PublishResolvedColumn
{
    std::optional<size_t> file_column;
    bool                  exact{false};
    std::string           required_variant; // the variant the printer needs
    std::string           used_variant;     // the variant file_column belongs to
};
PublishResolvedColumn publish_resolved_column(const DynamicPrintConfig &uncompacted, size_t slot);

// Rewrite variant-bearing material keys to "base#file_column" for the column the slot prints with,
// so a receiver can address the variant. Full entries keep their base keys (they carry every
// variant). Adds one notice per slot whose printer variant the preset does not define.
void publish_normalize_material_entries(std::vector<PublishedMaterialEntry> &entries,
                                        const DynamicPrintConfig &uncompacted,
                                        std::vector<std::string> *notices = nullptr);

// Clear the compatibility lists/conditions on a filament config so it is universally
// compatible once detached (empty lists + empty conditions = compatible with everything).
void make_publish_universal(DynamicPrintConfig &config);

// Naming base for a detached published-material copy: "Generic PLA @System" -> "Generic PLA"
// (truncate/right-trim at the first '@' tail). Empty result means "fall back to identity".
std::string publish_material_base_name(const std::string &preset_name);

// Minimal DynamicPrintConfig for a published 3MF export: only the selected published keys,
// material keys, identity fields and plate geometry keys.
DynamicPrintConfig filter_published_config(
    const DynamicPrintConfig &full_config,
    const std::vector<std::string> &published_keys,
    const std::vector<PublishedMaterialEntry> &material_keys);

// Receiver index for the author's per-variant index: the receiver's extruder with the author's
// extruder number, when it currently runs the author's nozzle type. -1 when it does not.
// The overload without a target resolves printer keys; the other looks the index up in
// receiver_target with its id/variant keys (process keys: print_extruder_id / print_extruder_variant).
int resolve_published_extruder_slot(const PublishedExtruderLayout &author, size_t author_index,
                                    const DynamicPrintConfig &receiver_printer, const DynamicPrintConfig &receiver_project);
int resolve_published_extruder_slot(const PublishedExtruderLayout &author, size_t author_index,
                                    const DynamicPrintConfig &receiver_printer, const DynamicPrintConfig &receiver_project,
                                    const DynamicPrintConfig &receiver_target, const std::string &id_key, const std::string &variant_key);
}
