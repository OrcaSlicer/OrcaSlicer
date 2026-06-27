#ifndef slic3r_VariantOverrides_hpp_
#define slic3r_VariantOverrides_hpp_

// ═══════════════════════════════════════════════════════════════════════════════
// H2C VariantOverrides — Isolated Variant Override Layer
// ═══════════════════════════════════════════════════════════════════════════════
//
// PROBLEM
// -------
// BambuLab multi-extruder printers (H2C) support different nozzle variants per
// physical extruder (e.g. Left=HighFlow, Right=Standard). Each variant can have
// its own speed/acceleration profile. The Bambu preset system stores these as
// JSON arrays in `print_options_with_variant`:
//
//   "inner_wall_speed": [300, 600, 300, 600]
//
// OrcaSlicer's core config uses SCALAR types (ConfigOptionFloat, etc.) for
// these options — they can only hold ONE value at a time.
//
// SOLUTION
// --------
// VariantOverrides stores the FULL per-variant arrays alongside the scalar
// config. It provides all operations for:
//   - Applying a variant's values into the scalar config (apply_to_config)
//   - Saving scalar edits back into the variant array (save_from_config)
//   - Expanding scalars+VO into vectors for JSON serialization (expand_to_vectors)
//   - Compressing JSON vectors back into scalars+VO after load (compress_from_vectors)
//   - Building per-extruder overlays for G-code generation (build_overlay)
//   - Precomputing all overlays at print start (precompute_overlays)
//
// ─── INDEX MAPPING CHAIN (H2C) ──────────────────────────────────────────────
//
// Three separate index spaces exist. ALL code MUST use the same mapping.
//
// 1) PHYSICAL EXTRUDER ID (0-based):
//      0 = MAIN  (firmware extruder 0, physical Right nozzle on H2C)
//      1 = DEPUTY (firmware extruder 1, physical Left nozzle on H2C)
//
// 2) UI POSITION → PHYSICAL EXTRUDER (via physical_extruder_map in printer preset):
//      physical_extruder_map = [1, 0]    // H2C preset
//        map[0] = 1 → Left UI  → extruder 1 (DEPUTY)
//        map[1] = 0 → Right UI → extruder 0 (MAIN)
//      Canonical API: left_extruder_idx(config), right_extruder_idx(config)
//
// 3) VARIANT INDEX (position in flattened VO / process arrays):
//      extruder_variant_list (printer preset):
//        [0] = "Direct Drive Standard,Direct Drive High Flow"   // ext 0 (MAIN)
//        [1] = "Direct Drive Standard,Direct Drive High Flow"   // ext 1 (DEPUTY)
//      Flattened: [ext0/Std=0, ext0/HF=1, ext1/Std=2, ext1/HF=3]
//
//      print_extruder_id / print_extruder_variant (process preset):
//        print_extruder_id      = [1, 1, 2, 2]            // 1-based ext ID
//        print_extruder_variant = [DD Std, DD HF, DD Std, DD HF]
//
//      Process arrays match flattened order:
//        "inner_wall_speed": [300, 600, 300, 600]
//                             ^^^  ^^^  ^^^  ^^^
//                           ext0/S ext0/HF ext1/S ext1/HF
//                           MAIN   MAIN    DEPUTY DEPUTY
//                           Right  Right   Left   Left
//
//      compute_variant_index(eid, config) and get_index_for_extruder()
//      both return this flattened position:
//        ext0 (MAIN/Right)  + Standard → variant_index 0
//        ext1 (DEPUTY/Left) + Standard → variant_index 2
//
// ─── FULL CHAIN EXAMPLE: Left toggle in Tab ─────────────────────────────────
//
//   Left toggle clicked
//     → left_extruder_idx(printer_config)                    → 1 (DEPUTY)
//     → get_index_for_extruder(1+1=2, "DD Standard")        → index 2
//     → save_from_config(VO, old_index)                      saves current edits
//     → apply_to_config(VO, 2)                               loads Left/Standard values
//     → m_last_variant_index = 2
//
//   Slice button
//     → precompute_overlays:
//         compute_variant_index(eid=1, config)               → vi=2
//         build_overlay(2)                                    reads VO[2]
//     → GCode toolchange to ext1: applies overlay from VO[2] ✓ matches Tab
//
// ─── DATA FLOW ──────────────────────────────────────────────────────────────
//
//   Profile JSON
//       |
//       v
//   compress_from_vectors() -- JSON arrays → scalar + VO storage
//       |
//       +→ Scalar config ← active variant value
//       +→ VariantOverrides ← all variant values
//       |
//   Tab switch (Left/Right nozzle):
//       save_from_config()  -- save current edits to VO
//       apply_to_config()   -- load new variant's values
//       |
//   JSON save:
//       expand_to_vectors() -- VO → JSON arrays
//       |
//   G-code generation:
//       precompute_overlays() → per-extruder DynamicPrintConfig overlays
//
// CORE INTEGRATION
// ────────────────
// DynamicPrintConfig holds a VariantOverrides member and thin wrapper methods.
// All logic lives HERE, core classes only have hooks.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>

namespace Slic3r {

class DynamicPrintConfig;
class ConfigBase;

// ────────────────────────────────────────────────────────────────
// Canonical key set — options that have per-variant values
// ────────────────────────────────────────────────────────────────
// Defined in VariantOverrides.cpp. Includes speed, acceleration,
// jerk, and advanced options that vary between nozzle variants.
extern std::set<std::string> print_options_with_variant;

// ────────────────────────────────────────────────────────────────
// PrecomputedOverlays — result of precompute_overlays()
// ────────────────────────────────────────────────────────────────
// Built once at print start, consumed by VariantAwareConfig in GCode
// to apply per-extruder speed/accel/jerk at each toolchange.
struct PrecomputedOverlays {
    // Global: physical extruder_id -> overlay config
    std::map<unsigned int, DynamicPrintConfig> extruder_overrides;
    // Per-object: model_object_id -> extruder_id -> overlay config
    // Per-object overlays inherit from global, then override with object VO.
    std::map<size_t, std::map<unsigned int, DynamicPrintConfig>> object_extruder_overrides;
};

// ────────────────────────────────────────────────────────────────
// VariantOverrides — per-variant value storage + all operations
// ────────────────────────────────────────────────────────────────
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
    // ── Data ──
    std::map<std::string, std::vector<double>>          floats;
    std::map<std::string, std::vector<std::string>>     strings;

    // ── Accessors ──

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

    // ── Left/Right extruder mapping (canonical source of truth) ──

    // Returns the physical extruder ID for the Left / Right nozzle.
    // Reads physical_extruder_map from printer preset config (data-driven, like BBS).
    //   H2C preset: physical_extruder_map = [1, 0]
    //     → left_extruder_idx = 1 (DEPUTY), right_extruder_idx = 0 (MAIN)
    // ALL code must use these — no ad-hoc inline ternaries elsewhere.
    static int left_extruder_idx(const ConfigBase& config);
    static int right_extruder_idx(const ConfigBase& config);

    // ── Multi-variant detection ──

    // Check if a config has multiple nozzle variants (i.e. VO is non-empty
    // or *_extruder_variant options have >1 entry). Replaces 3x duplicated
    // inline checks in Tab.cpp switch_excluder / save_preset / on_value_change.
    static bool is_multi_variant(const DynamicPrintConfig& config);

    // ── Config ↔ VO operations ──

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

    // ── Overlay building (for G-code generation) ──

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

    // ── Variant index computation ──

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

    // ── 3MF I/O layer ──
    // Internal VO order matches BBS 3MF file order:
    //   [Left/v0, Left/v1, Right/v0, Right/v1]
    // No swap needed — ordering is determined by print_extruder_id from BBS preset.

    // [DEPRECATED] Swap extruder halves in VO arrays (rotate by n/2).
    // No longer called — internal order == BBS file order.
    void swap_extruder_order();

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
    // Parses CSV, swaps from BBS to internal order, stores in VO, sets scalar.
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

    // ── Debug ──

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
