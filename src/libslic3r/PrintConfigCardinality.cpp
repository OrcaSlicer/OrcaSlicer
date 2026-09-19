#include "PrintConfigCardinality.hpp"

#include "PrintConfig.hpp"
#include "Preset.hpp"

#include <algorithm>
#include <cassert>
#include <map>
#include <set>

namespace Slic3r {

// ===================== Classification: the key sets and the map they build =====================

// One key set per cardinality. The *_extra and custom_keys sets below are cardinality's own, spelled out one key per
// line so they are the discoverable place to see - and change - a classification. The variant / GUI /
// preset sets are owned by other subsystems and are referenced (not copied) in build_cardinality_map, so they
// cannot drift; see the comments there.

// Per-extruder to cardinality but absent from the GUI per-extruder set (printer_extruder_options).
static const std::set<std::string> per_extruder_extra_keys = {
    "extruder_offset",
    "extruder_colour",
};

// Per-filament to cardinality but not filament-preset fields, so absent from Preset::filament_options().
static const std::set<std::string> per_filament_extra_keys = {
    "filament_map",             // raw indexed read @ ToolOrdering.cpp
    "filament_colour",          // deliberately commented out of filament_options()
    "filament_colour_type",
    "filament_multi_colour",
};

// Vector options the sizer must NOT resize (matrices, geometry, bookkeeping). Listed explicitly so
// build_cardinality_map classifies every vector option: a new vector option that is neither per-N nor here
// trips the [Cardinality] completeness test rather than silently going unsized. This is also the audit surface
// - the LATENT group at the end are per-N in disguise, candidates to promote to a real cardinality.
static const std::set<std::string> custom_keys = {
    // Build-area geometry and point lists (owner-sized).
    "bed_exclude_area",
    "printable_area",
    "parallel_printheads_bed_exclude_areas",
    "wrapping_exclude_area",
    "head_wrap_detect_zone",
    "start_end_points",
    "grab_length",

    // Flush / wiping volumes (matrices and multipliers, not one-per-N).
    "flush_volumes_matrix",
    "flush_volumes_vector",
    "flush_multiplier",
    "wiping_volumes_extruders",

    // Compatibility and preset / profile bookkeeping.
    "compatible_prints",
    "compatible_printers",
    "compatible_machine_expression_group",
    "compatible_process_expression_group",
    "inherits_group",
    "different_settings_to_system",
    "print_compatible_printers",
    "upward_compatible_machine",
    "preset_names",
    "filament_settings_id",
    "default_filament_profile",
    "filament_ids",
    "extruder_ams_count",

    // Print-sequence, SLA corrections, post-process, and other owner-sized vectors.
    "first_layer_print_sequence",
    "other_layers_print_sequence",
    "material_correction",
    "relative_correction",
    "post_process",
    "wipe_tower_x",
    "wipe_tower_y",
    "small_area_infill_flow_compensation_model",

    // Plugin name/capability lists (print options, not one-per-N).
    "plugins",
    "slicing_pipeline_plugin",

    // --- LATENT per-N: currently Custom to preserve behavior, but each is really per-N. Promote in a follow-up. ---
    "extruder_variant_list",        // per-extruder
    "physical_extruder_map",        // per-extruder
    "nozzle_volume_type",           // per-extruder
    "extruder_nozzle_count",        // per-extruder
    "extruder_nozzle_volume_type",  // per-extruder
    "extruder_nozzle_stats",        // per-extruder
    "deretract_speed_extruder_change", // per-extruder (printer field, not in printer_extruder_options)
    "filament_self_index",          // per-filament
    "filament_map_2",               // per-filament (sibling of filament_map)
    "filament_nozzle_map",          // per-filament
    "filament_volume_map",          // per-filament
    "filament_max_temperature_drop_when_ec", // per-filament
    "flush_multiplier_fast",        // flush multiplier, not one-per-N (sibling of flush_multiplier)
    "machine_min_extruding_rate",   // sibling of the PerExtruderVariantDual machine_max_*
    "machine_min_travel_rate",      // sibling of the PerExtruderVariantDual machine_max_*

    // small_support_perimeter_* mirror small_perimeter_* (PerProcessVariant) but are absent from
    // print_options_with_variant on main, so are never variant-expanded; consumers read them via get_at(),
    // so the size-1 array is safe. Kept Custom to preserve current behavior.
    "small_support_perimeter_speed",
    "small_support_perimeter_threshold",
};

// The "plain" per-filament options: vector-typed filament_options() that are neither variant-expanded nor
// Custom. A pure function of static schema, so it is cached on first use.
static const std::set<std::string> &plain_per_filament_options()
{
    static const std::set<std::string> keys = [] {
        std::set<std::string> k;
        for (const std::string &key : Preset::filament_options()) {
            if (filament_options_with_variant.count(key) || custom_keys.count(key) || filament_dev_options.count(key))
                continue;
            const ConfigOptionDef *def = print_config_def.get(key);
            if (def && def->default_value && def->default_value->is_vector())
                k.insert(key);
        }
        return k;
    }();
    return keys;
}

static std::map<std::string, ConfigCardinality> build_cardinality_map()
{
    std::map<std::string, ConfigCardinality> m;
    // A key may appear in several sets but must resolve to one cardinality (a conflict asserts), so order is irrelevant.
    auto classify_all = [&m](const std::set<std::string> &keys, ConfigCardinality a) {
        for (const std::string &k : keys) {
            const auto [it, inserted] = m.emplace(k, a);
            assert(inserted || it->second == a);
        }
    };

    // One classify_all per cardinality. "borrowed" sets are the subsystem's own (referenced, so they can't drift);
    // the *_extra / custom_keys sets are cardinality's own.
    classify_all(printer_extruder_options,       ConfigCardinality::PerExtruder);            // borrowed: GUI per-extruder
    classify_all(per_extruder_extra_keys,        ConfigCardinality::PerExtruder);            // cardinality-owned
    classify_all(printer_options_with_variant_1, ConfigCardinality::PerExtruderVariant);     // borrowed: variant expansion
    classify_all(printer_options_with_variant_2, ConfigCardinality::PerExtruderVariantDual); // borrowed: variant expansion
    classify_all(plain_per_filament_options(),   ConfigCardinality::PerFilament);            // borrowed: filament schema
    classify_all(per_filament_extra_keys,        ConfigCardinality::PerFilament);            // cardinality-owned
    classify_all(filament_options_with_variant,  ConfigCardinality::PerFilamentVariant);     // borrowed: variant expansion
    classify_all(print_options_with_variant,     ConfigCardinality::PerProcessVariant);      // borrowed: variant expansion
    classify_all(custom_keys,                    ConfigCardinality::Custom);                 // cardinality-owned
    classify_all(filament_dev_options,           ConfigCardinality::Custom);                 // borrowed: dev/AMS drying, excluded from filament sizing upstream
    return m;
}

ConfigCardinality cardinality_of(const std::string &opt_key)
{
    // Built on first use, so the sizing sets and print_config_def are already constructed.
    static const std::map<std::string, ConfigCardinality> m = build_cardinality_map();
    auto it = m.find(opt_key);
    return it == m.end() ? ConfigCardinality::Scalar : it->second;
}

// ===================== Counts and expected lengths =====================

// Returns `fallback` (not 0) for an empty or absent option: expected_size reads 0 as "not per-N", so a 0
// here would make enforce_cardinality skip it.
static size_t vector_size_or(const ConfigBase &config, const std::string &key, size_t fallback)
{
    const ConfigOption *o = config.option(key);
    if (o == nullptr || !o->is_vector())
        return fallback;
    const auto *vec = static_cast<const ConfigOptionVectorBase *>(o);
    return vec->empty() ? fallback : vec->size();
}

AxisCounts axis_counts(const ConfigBase &config)
{
    AxisCounts n;
    n.extruders = vector_size_or(config, "nozzle_diameter",   n.extruders);
    n.filaments = vector_size_or(config, "filament_diameter", n.filaments);

    // A variant list normally holds at least one slot per extruder/filament (more with multiple variants per
    // slot), so take its size but never below the base count: an absent or stale (default size-1) list must
    // not shrink per-variant options below the extruder/filament count. On the normal path the max() is a no-op.
    n.extruder_variant = std::max(vector_size_or(config, "printer_extruder_variant",  n.extruder_variant), n.extruders);
    n.filament_variant = std::max(vector_size_or(config, "filament_extruder_variant", n.filament_variant), n.filaments);
    n.process_variant  = std::max(vector_size_or(config, "print_extruder_variant",    n.process_variant),  n.extruders);
    return n;
}

size_t cardinality_stride(ConfigCardinality cardinality)
{
    return cardinality == ConfigCardinality::PerExtruderVariantDual ? 2 : 1;
}

size_t expected_size(ConfigCardinality cardinality, const AxisCounts &n)
{
    switch (cardinality) {
    case ConfigCardinality::PerExtruder:            return n.extruders;
    case ConfigCardinality::PerFilament:            return n.filaments;
    case ConfigCardinality::PerExtruderVariant:     return n.extruder_variant;
    case ConfigCardinality::PerExtruderVariantDual: return n.extruder_variant * cardinality_stride(cardinality);
    case ConfigCardinality::PerFilamentVariant:     return n.filament_variant;
    case ConfigCardinality::PerProcessVariant:      return n.process_variant;
    case ConfigCardinality::Scalar:
    case ConfigCardinality::Custom:                 return 0; // not per-N: the sizer leaves it alone
    }
    return 0;
}

// Per-N options a config authors to a length that contradicts its axis count (see the header). An option still
// at its schema default was not authored, so coercion may legitimately broadcast it - only a set wrong length counts.
std::vector<CardinalityMismatch> cardinality_contradictions(const ConfigBase &config)
{
    std::vector<CardinalityMismatch> bad;
    const AxisCounts n = axis_counts(config);
    for (const std::string &key : config.keys()) {
        const size_t want = expected_size(cardinality_of(key), n);
        if (want == 0)                                    // Scalar / Custom: not per-N
            continue;
        const ConfigOption *opt = config.option(key);
        if (opt == nullptr || !opt->is_vector())
            continue;
        const size_t got = static_cast<const ConfigOptionVectorBase *>(opt)->size();
        if (got == want)
            continue;
        const ConfigOptionDef *def = print_config_def.get(key);
        if (def != nullptr && def->default_value && *opt == *def->default_value)
            continue;
        bad.push_back({ key, got, want });
    }
    return bad;
}

// ===================== Applying cardinality: resize helpers =====================

// Resize to exactly `want`. An unset (empty) option with no default to replicate is left alone - "unset"
// is distinct from "wrong length".
static void resize_option(ConfigOption *opt, size_t want, const std::string &key)
{
    if (opt == nullptr || !opt->is_vector())
        return;
    auto *vec = static_cast<ConfigOptionVectorBase *>(opt);
    if (vec->size() == want)
        return;
    const ConfigOption *def = FullPrintConfig::defaults().option(key);
    if (vec->empty()) {
        const bool default_empty = def == nullptr || !def->is_vector()
            || static_cast<const ConfigOptionVectorBase *>(def)->empty();
        if (default_empty)
            return; // some options (e.g. extruder_printable_area) have an empty default
    }
    vec->resize(want, def);
}

// Resize one already-fetched option to the length its cardinality implies; Scalar/Custom (want 0) are left alone.
static void resize_to_cardinality(ConfigOption *opt, const std::string &key, const AxisCounts &n)
{
    const size_t want = expected_size(cardinality_of(key), n);
    if (want != 0)
        resize_option(opt, want, key);
}

// Per-key sizer: look the option up, then apply the same rule as enforce_cardinality.
void resize_to_cardinality(ConfigBase &config, const std::string &key, const AxisCounts &n)
{
    resize_to_cardinality(config.option(key, false), key, n);
}

// ===================== DynamicPrintConfig entry points (declared in PrintConfig.hpp) =====================

void DynamicPrintConfig::enforce_cardinality()
{
    const AxisCounts n = axis_counts(*this);
    for (auto it = this->cbegin(); it != this->cend(); ++it)
        resize_to_cardinality(it->second.get(), it->first, n);
}

void DynamicPrintConfig::enforce_per_filament_cardinality()
{
    const AxisCounts n = axis_counts(*this);
    for (const std::string &key : plain_per_filament_options())
        resize_to_cardinality(this->option(key, false), key, n);
}

// Legacy per-key shim on the set_num_extruders path: the length param_name should have for extruder_nums
// extruders. New code should prefer enforce_cardinality() / expected_size().
size_t DynamicPrintConfig::get_parameter_size(const std::string &param_name, size_t extruder_nums)
{
    AxisCounts n = axis_counts(*this);
    n.extruders = extruder_nums;
    const size_t size = expected_size(cardinality_of(param_name), n);

    // expected_size returns 0 for Scalar/Custom, but the caller resizes to the result, so fall back to the
    // extruder count (historical behavior) instead of clearing the option with resize(0).
    return size != 0 ? size : extruder_nums;
}

} // namespace Slic3r
