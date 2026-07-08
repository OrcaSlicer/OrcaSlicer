#ifndef slic3r_VariantOverrides_hpp_
#define slic3r_VariantOverrides_hpp_

// H2C VariantOverrides - Isolated Variant Override Layer
//
// H2C printers support per-extruder nozzle variants (e.g. Left=HighFlow, Right=Standard).
// The Bambu preset system stores per-variant arrays in JSON
// (e.g. "inner_wall_speed": [300, 600, 300, 600]) while OrcaSlicer uses scalar types.
// VariantOverrides stores the full per-variant arrays alongside the scalar config and
// provides: apply_to_config, save_from_config, expand_to_vectors, compress_from_vectors,
// build_overlay, precompute_overlays.
//
// Variant index is flattened over extruder*variant_count:
//   ext0 (MAIN/Right) Standard=0, HighFlow=1; ext1 (DEPUTY/Left) Standard=2, HighFlow=3.
// Use left_extruder_idx(config)/right_extruder_idx(config) for UI->extruder mapping.
//
// DynamicPrintConfig holds a VariantOverrides member; all logic lives here.

#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>

namespace Slic3r {

class DynamicPrintConfig;
class ConfigBase;

// 
// Canonical key set  -  options that have per-variant values
// 
// Defined in VariantOverrides.cpp. Includes speed, acceleration,
// jerk, and advanced options that vary between nozzle variants.
extern std::set<std::string> print_options_with_variant;

// 
// PrecomputedOverlays  -  result of precompute_overlays()
// 
// Built once at print start, consumed by VariantAwareConfig in GCode
// to apply per-extruder speed/accel/jerk at each toolchange.
struct PrecomputedOverlays {
    // Global: physical extruder_id -> overlay config
    std::map<unsigned int, DynamicPrintConfig> extruder_overrides;
    // Per-object: model_object_id -> extruder_id -> overlay config
    // Per-object overlays inherit from global, then override with object VO.
    std::map<size_t, std::map<unsigned int, DynamicPrintConfig>> object_extruder_overrides;
};

// 
// VariantOverrides  -  per-variant value storage + all operations
// 
//
// STORAGE FORMAT
//   floats:  key -> [variant0_val, variant1_val, ...]  (numeric values)
//   strings: key -> [variant0_str, variant1_str, ...]  (preserves "50%" notation)
//
// VARIANT INDEXING (see INDEX MAPPING CHAIN above for full details)
//   VO arrays are ordered by extruder_variant_list (flattened):
//     ext0_var0, ext0_var1, ext1_var0, ext1_var1, ...
//   For H2C Standard+HighFlow:
//     [0]=MAIN(Right)/Std, [1]=MAIN(Right)/HF, [2]=DEPUTY(Left)/Std, [3]=DEPUTY(Left)/HF
//   compute_variant_index() and get_index_for_extruder() return positions in this order.
//
struct VariantOverrides {
    //  Data 
    std::map<std::string, std::vector<double>>          floats;
    std::map<std::string, std::vector<std::string>>     strings;

    //  Accessors 

    // Check if a key has variant override values stored.
    bool        has(const std::string& key) const;
    // True if no variant overrides are stored at all.
    bool        empty() const;
    // Remove all stored variant overrides.
    void        clear();
    // Get numeric value for key at variant index (clamps to 0 if out of range).
    double      get_float(const std::string& key, int index) const;
    // Get raw string value (preserves "50%" percent notation).
    std::string get_string(const std::string& key, int index) const;
    // Number of variant values stored for a key (0 if key not present).
    int         variant_count(const std::string& key) const;
    // Write a numeric value at variant index (no-op if key/index invalid).
    void        set_float(const std::string& key, int index, double value);
    // Write a string value at variant index (no-op if key/index invalid).
    void        set_string(const std::string& key, int index, const std::string& value);
    // Copy a single key's float+string arrays from another VO.
    // Used to sync per-object overrides without exposing floats/strings directly.
    void        copy_key_from(const std::string& key, const VariantOverrides& source);
    // Erase a single key from both floats and strings maps (ALL variants).
    void        erase_key(const std::string& key);
    // Erase a single variant slot for a key (sets NaN sentinel).
    // If ALL slots become NaN, removes the key entirely.
    // Used by per-object reset: erase only the active extruder's override.
    void        erase_variant(const std::string& key, int variant_idx);
    // Check if a specific variant slot is set (not NaN sentinel).
    bool        has_variant(const std::string& key, int variant_idx) const;

    //  Left/Right extruder mapping (canonical source of truth) 

    // Returns the physical extruder ID for the Left / Right nozzle.
    // Reads physical_extruder_map from printer preset config (data-driven, like BBS).
    //   H2C preset: physical_extruder_map = [1, 0]
    //      left_extruder_idx = 1 (DEPUTY), right_extruder_idx = 0 (MAIN)
    // ALL code must use these  -  no ad-hoc inline ternaries elsewhere.
    static int left_extruder_idx(const ConfigBase& config);
    static int right_extruder_idx(const ConfigBase& config);

    //  Multi-variant detection 

    // Check if a config has multiple nozzle variants (i.e. VO is non-empty
    // or *_extruder_variant options have >1 entry). Replaces 3x duplicated
    // inline checks in Tab.cpp switch_excluder / save_preset / on_value_change.
    static bool is_multi_variant(const DynamicPrintConfig& config);

    //  Config ↔ VO operations 

    // Apply variant values from this VO into scalar config options.
    // For each key in `keys`: reads VO[variant_index] and writes to config scalar.
    // Auto-initializes missing VO entries by replicating current scalar across all slots.
    // Called on: Tab variant switch (to load the new variant's values).
    void apply_to_config(DynamicPrintConfig& config, int variant_index,
                         const std::set<std::string>& keys);

    // Save current scalar config values back into this VO at variant_index.
    // Preserves user edits when switching between Left/Right nozzle tabs.
    // Auto-initializes missing VO entries (same as apply_to_config).
    // When force=true, overwrites NaN/reset slots (for explicit user edits).
    // When force=false, preserves NaN/reset state (for automatic tab switches).
    void save_from_config(const DynamicPrintConfig& config, int variant_index,
                          const std::set<std::string>& keys, bool force = false);

    // Convert scalar config + VO arrays into vector ConfigOptions.
    // Used before JSON serialization so save_to_json() produces BBS-compatible
    // arrays like "inner_wall_speed": [300, 600, 300, 600].
    // Clears this VO after expansion (values now live in config vectors).
    void expand_to_vectors(DynamicPrintConfig& config);

    // Inverse of expand: reads vector ConfigOptions loaded from JSON,
    // stores all values in this VO, replaces config with scalar at active_variant_index.
    // Called after loading a BBS-style preset with array values.
    void compress_from_vectors(DynamicPrintConfig& config, int active_variant_index);

    //  Overlay building (for G-code generation) 

    // Build a DynamicPrintConfig containing only the overridden scalar values
    // for a specific variant_index. Applied on top of the base config at toolchange.
    // If base_overlay is non-null, starts from a copy (merge semantics for per-object).
    DynamicPrintConfig build_overlay(int variant_index,
                                     const DynamicPrintConfig* base_overlay = nullptr) const;

    // Build all per-extruder and per-object overlays in one pass.
    // Called once at print start by GCode::precompute_extruder_speed_overrides().
    // object_vos: pairs of (model_object_id, per-object VariantOverrides pointer).
    static PrecomputedOverlays precompute_overlays(
        const VariantOverrides& global_vo,
        const ConfigBase&       full_config,
        unsigned int            num_extruders,
        const std::vector<std::pair<size_t, const VariantOverrides*>>& object_vos);

    //  Variant index computation 

    // Compute the VO array index for a physical extruder given its type/nozzle.
    // Maps (extruder_id, extruder_type, nozzle_volume_type) to the correct
    // position in the flattened VO array using extruder_variant_list.
    // Returns -1 if not found (single-extruder printer or unknown variant).
    static int compute_variant_index(
        unsigned int extruder_id,
        int          extruder_type,
        int          nozzle_volume_type,
        const std::vector<std::string>& extruder_variant_list);

    // Convenience: reads extruder_type, nozzle_volume_type, and
    // extruder_variant_list from the config automatically.
    static int compute_variant_index(unsigned int extruder_id, const ConfigBase& config);

    //  3MF I/O layer 
    // Internal VO order matches BBS 3MF file order:
    //   [Left/v0, Left/v1, Right/v0, Right/v1]
    // No swap needed  -  ordering is determined by print_extruder_id from BBS preset.

    // Prepare a DynamicPrintConfig for 3MF save:
    //   Expands VO into vector ConfigOptions (no swap needed).
    // Config is modified in-place (caller should pass a copy).
    static void prepare_for_3mf_save(DynamicPrintConfig& config);

    // Load global/embedded preset from 3MF JSON:
    //   Compresses vector ConfigOptions into VO (no swap needed).
    // Called after load_from_json().
    static void load_from_3mf_compress(DynamicPrintConfig& config, int active_variant_index);

    // Check if a 3MF metadata key/value is a variant-aware CSV.
    // Returns true if the key should be SKIPPED by the first-pass set_deserialize
    // (it will be handled by try_load_per_object_3mf_metadata instead).
    static bool is_variant_csv(const std::string& key, const std::string& value);

    // Parse and load a per-object variant-aware CSV from 3MF metadata.
    // Parses CSV, stores values in VO, sets scalar.
    // Call in second pass for all metadata where is_variant_csv() returned true.
    static void try_load_per_object_3mf_metadata(
        const std::string& key, const std::string& value,
        DynamicPrintConfig& config);

    // Parse a comma-separated value string for a variant-aware key.
    // BBS 3MF files store per-variant values as "300,600,300,600".
    // Returns vector of doubles if key is variant-aware AND value has commas.
    // Returns nullopt otherwise (not a variant key or scalar value).
    static std::optional<std::vector<double>> parse_variant_csv(
        const std::string& key,
        const std::string& value);

    //  Debug 

    // Log all VO contents via BOOST_LOG_TRIVIAL(info) with given prefix.
    void dump(const std::string& prefix) const;

private:
    // Infer the total number of variant slots.
    // Checks existing VO array sizes first; falls back to reading
    // *_extruder_variant config options for empty-VO edge cases.
    static int determine_variant_count(const DynamicPrintConfig& config,
                                       const VariantOverrides& vo);
};

} // namespace Slic3r

#endif // slic3r_VariantOverrides_hpp_
