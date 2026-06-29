#include "VariantOverrides.hpp"
#include "PrintConfig.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <sstream>
#include <cmath>
#include <limits>

namespace Slic3r {

// ────────────────────────────────────────────────────────────────
// Canonical key sets
// ────────────────────────────────────────────────────────────────
std::set<std::string> print_options_with_variant = {
    // --- Speeds ---
    "initial_layer_speed",
    "initial_layer_infill_speed",
    "initial_layer_travel_speed",
    "slow_down_layers",
    "outer_wall_speed",
    "inner_wall_speed",
    "small_perimeter_speed",
    "small_perimeter_threshold",
    "sparse_infill_speed",
    "internal_solid_infill_speed",
    "top_surface_speed",
    "enable_overhang_speed",
    "slowdown_for_curled_perimeters",
    "overhang_1_4_speed",
    "overhang_2_4_speed",
    "overhang_3_4_speed",
    "overhang_4_4_speed",
    "bridge_speed",
    "internal_bridge_speed",
    "gap_infill_speed",
    "support_speed",
    "support_interface_speed",
    "travel_speed",
    "travel_speed_z",
    // --- Acceleration ---
    "default_acceleration",
    "initial_layer_acceleration",
    "initial_layer_travel_acceleration",
    "outer_wall_acceleration",
    "inner_wall_acceleration",
    "sparse_infill_acceleration",
    "internal_solid_infill_acceleration",
    "bridge_acceleration",
    "top_surface_acceleration",
    "travel_acceleration",
    // --- Jerk ---
    "default_jerk",
    "outer_wall_jerk",
    "inner_wall_jerk",
    "top_surface_jerk",
    "infill_jerk",
    "initial_layer_jerk",
    "travel_jerk",
    "initial_layer_travel_jerk",
    "default_junction_deviation",
    // --- Advanced ---
    "max_volumetric_extrusion_rate_slope",
    "max_volumetric_extrusion_rate_slope_segment_length",
    "extrusion_rate_smoothing_external_perimeter_only",
    // --- Extruder identity ---
    "print_extruder_id",
    "print_extruder_variant"
};

// ────────────────────────────────────────────────────────────────
// VariantOverrides — accessors
// ────────────────────────────────────────────────────────────────
bool VariantOverrides::has(const std::string& key) const { return floats.count(key) > 0; }
bool VariantOverrides::empty() const { return floats.empty(); }
void VariantOverrides::clear() { floats.clear(); strings.clear(); }

double VariantOverrides::get_float(const std::string& key, int index) const {
    const auto it = floats.find(key);
    if (it == floats.end() || it->second.empty()) return 0.0;
    int idx = (index >= 0 && index < (int)it->second.size()) ? index : 0;
    return it->second[idx];
}

std::string VariantOverrides::get_string(const std::string& key, int index) const {
    const auto it = strings.find(key);
    if (it == strings.end() || it->second.empty()) return {};
    int idx = (index >= 0 && index < (int)it->second.size()) ? index : 0;
    return it->second[idx];
}

int VariantOverrides::variant_count(const std::string& key) const {
    const auto it = floats.find(key);
    return it != floats.end() ? (int)it->second.size() : 0;
}

void VariantOverrides::set_float(const std::string& key, int index, double value) {
    auto it = floats.find(key);
    if (it != floats.end() && index >= 0 && index < (int)it->second.size())
        it->second[index] = value;
}

void VariantOverrides::set_string(const std::string& key, int index, const std::string& value) {
    auto it = strings.find(key);
    if (it != strings.end() && index >= 0 && index < (int)it->second.size())
        it->second[index] = value;
}

void VariantOverrides::copy_key_from(const std::string& key, const VariantOverrides& source) {
    // Copy float array for this key (or erase if source doesn't have it)
    if (const auto it = source.floats.find(key); it != source.floats.end())
        floats[key] = it->second;
    else
        floats.erase(key);
    // Copy string array for this key
    if (const auto it = source.strings.find(key); it != source.strings.end())
        strings[key] = it->second;
    else
        strings.erase(key);
}

void VariantOverrides::erase_key(const std::string& key) {
    floats.erase(key);
    strings.erase(key);
}

void VariantOverrides::erase_variant(const std::string& key, int variant_idx) {
    if (variant_idx < 0) return;
    auto fit = floats.find(key);
    if (fit != floats.end() && (size_t)variant_idx < fit->second.size()) {
        fit->second[variant_idx] = std::numeric_limits<double>::quiet_NaN();
        auto sit = strings.find(key);
        if (sit != strings.end() && (size_t)variant_idx < sit->second.size())
            sit->second[variant_idx].clear();
        bool all_nan = true;
        for (double v : fit->second) {
            if (!std::isnan(v)) { all_nan = false; break; }
        }
        if (all_nan) {
            erase_key(key);
        }
    } else {
    }
}

bool VariantOverrides::has_variant(const std::string& key, int variant_idx) const {
    if (variant_idx < 0) return false;
    auto fit = floats.find(key);
    if (fit == floats.end() || (size_t)variant_idx >= fit->second.size())
        return false;
    return !std::isnan(fit->second[variant_idx]);
}

// ────────────────────────────────────────────────────────────────
// Left/Right → physical extruder ID  (data-driven from preset)
// ────────────────────────────────────────────────────────────────
// Reads physical_extruder_map from printer preset config.
// H2C preset: physical_extruder_map = [1, 0]
//   → left  = map[0] = 1 (DEPUTY)
//   → right = map[1] = 0 (MAIN)
int VariantOverrides::left_extruder_idx(const ConfigBase& config)
{
    auto* map = config.option<ConfigOptionInts>("physical_extruder_map");
    if (map && map->values.size() >= 2)
        return map->values[0];
    return 0;  // single-extruder fallback
}

int VariantOverrides::right_extruder_idx(const ConfigBase& config)
{
    auto* map = config.option<ConfigOptionInts>("physical_extruder_map");
    if (map && map->values.size() >= 2)
        return map->values[1];
    return 0;  // single-extruder fallback
}

bool VariantOverrides::is_multi_variant(const DynamicPrintConfig& config) {
    // Fast path: VO is already populated
    if (!config.variant_overrides().empty())
        return true;
    // Fallback: check if any *_extruder_variant option has >1 entry
    for (const char* vkey : {"print_extruder_variant", "filament_extruder_variant", "printer_extruder_variant"}) {
        const auto* opt = dynamic_cast<const ConfigOptionStrings*>(config.option(vkey));
        if (opt && (int)opt->size() > 1)
            return true;
    }
    return false;
}

// ────────────────────────────────────────────────────────────────
// Internal helper: determine how many variant slots exist.
// ────────────────────────────────────────────────────────────────
// First tries to infer from existing VO arrays (most common case).
// Fallback: reads variant count from *_extruder_variant config options.
// This handles the edge case where a preset has only scalar values
// (no arrays), so VO starts empty but we still need to know the count.
int VariantOverrides::determine_variant_count(const DynamicPrintConfig& config, const VariantOverrides& vo)
{
    // 1) Check existing VO arrays — the largest array size is the variant count
    int vc = 0;
    for (const auto& [k, v] : vo.floats)
        vc = std::max(vc, (int)v.size());

    // 2) Fallback: read from the variant list config options themselves
    if (vc == 0) {
        for (const char* vkey : {"print_extruder_variant", "filament_extruder_variant", "printer_extruder_variant"}) {
            const auto* opt = dynamic_cast<const ConfigOptionStrings*>(config.option(vkey));
            if (opt && (int)opt->size() > 1) { vc = (int)opt->size(); break; }
        }
    }
    return vc;
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::compute_variant_index
// ────────────────────────────────────────────────────────────────
// Maps a physical extruder to its position in the flattened VO array.
//
// extruder_variant_list stores comma-separated variant names per extruder:
//   extruder_variant_list[0] = "DirectDrive_Standard,DirectDrive_HighFlow"
//   extruder_variant_list[1] = "DirectDrive_Standard,DirectDrive_HighFlow"
//
// The VO array is flattened in order:
//   [0]=ext0/var0, [1]=ext0/var1, [2]=ext1/var0, [3]=ext1/var1
//
// We walk through extruders, accumulating the offset (vo_offset), then
// find the matching variant name within the target extruder's list.
// Returns -1 if the extruder or variant is not found.
int VariantOverrides::compute_variant_index(
    unsigned int extruder_id,
    int          extruder_type,
    int          nozzle_volume_type,
    const std::vector<std::string>& extruder_variant_list)
{
    std::string search_variant = get_extruder_variant_string(
        (ExtruderType)extruder_type, (NozzleVolumeType)nozzle_volume_type);

    int vo_offset = 0;
    for (unsigned int e = 0; e < extruder_variant_list.size(); ++e) {
        std::vector<std::string> variants;
        boost::split(variants, extruder_variant_list[e],
                     boost::is_any_of(","), boost::token_compress_on);
        if (e == extruder_id) {
            for (int v = 0; v < (int)variants.size(); ++v) {
                std::string trimmed = variants[v];
                boost::trim(trimmed);
                if (trimmed == search_variant)
                    return vo_offset + v;
            }
            return -1;
        }
        vo_offset += (int)variants.size();
    }
    return -1;
}

int VariantOverrides::compute_variant_index(unsigned int extruder_id, const ConfigBase& config)
{
    const auto* evl = dynamic_cast<const ConfigOptionStrings*>(config.option("extruder_variant_list"));
    if (!evl) return -1;

    const auto* opt_ext_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(config.option("extruder_type"));
    const auto* opt_nvt      = dynamic_cast<const ConfigOptionEnumsGeneric*>(config.option("nozzle_volume_type"));

    int et  = opt_ext_type ? opt_ext_type->get_at(extruder_id) : (int)etDirectDrive;
    int nvt = opt_nvt      ? opt_nvt->get_at(extruder_id)      : (int)nvtStandard;

    return compute_variant_index(extruder_id, et, nvt, evl->values);
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::build_overlay
// ────────────────────────────────────────────────────────────────
// Creates a lightweight DynamicPrintConfig containing ONLY the overridden
// scalar values for a specific variant_index. This overlay is applied
// on top of the base FullPrintConfig at each toolchange during G-code
// generation (see VariantAwareConfig::reapply_variant_overrides).
//
// For per-object overlays, base_overlay is the global extruder overlay,
// so per-object values override global values (merge semantics).
//
// Only processes keys in print_options_with_variant.
// Handles coFloat, coFloatOrPercent (preserves %), coBool, coInt.
DynamicPrintConfig VariantOverrides::build_overlay(
    int variant_index,
    const DynamicPrintConfig* base_overlay) const
{
    DynamicPrintConfig overlay;
    if (base_overlay)
        overlay = *base_overlay;

    if (empty() || variant_index < 0)
        return overlay;

    for (const auto& key : print_options_with_variant) {
        if (!has_variant(key, variant_index)) {
            continue;
        }
        const ConfigOptionDef* optdef = print_config_def.get(key);
        if (!optdef) continue;

        switch (optdef->type) {
        case coFloat:
            overlay.set_key_value(key, new ConfigOptionFloat(get_float(key, variant_index)));
            break;
        case coFloatOrPercent: {
            std::string raw = get_string(key, variant_index);
            if (!raw.empty()) {
                auto* fop = new ConfigOptionFloatOrPercent();
                if (raw.back() == '%') {
                    fop->value = std::stod(raw.substr(0, raw.size() - 1));
                    fop->percent = true;
                } else {
                    fop->value = std::stod(raw);
                    fop->percent = false;
                }
                overlay.set_key_value(key, fop);
            }
            break;
        }
        case coBool:
            overlay.set_key_value(key, new ConfigOptionBool(get_float(key, variant_index) != 0.0));
            break;
        case coInt:
            overlay.set_key_value(key, new ConfigOptionInt((int)get_float(key, variant_index)));
            break;
        default: break;
        }
    }
    return overlay;
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::apply_to_config
// ────────────────────────────────────────────────────────────────
// Reads VO[variant_index] for each key and writes to the scalar ConfigOption.
//
// AUTO-INIT: If a key has no VO entry yet (happens when preset JSON had
// a scalar value, not an array), we replicate the current scalar value
// across ALL variant slots. This ensures each variant can be edited
// independently from the first tab switch onward.
//
// Called by DynamicPrintConfig::apply_variant_overrides() on tab switch.
void VariantOverrides::apply_to_config(DynamicPrintConfig& config, int variant_index,
                                        const std::set<std::string>& keys)
{
    const ConfigDef* config_def = config.def();
    if (!config_def) return;

    int variant_count = determine_variant_count(config, *this);
    if (variant_count <= 0) return;

    for (const auto& key : keys) {
        ConfigOption* opt = config.option(key, false);
        if (!opt) continue;
        const ConfigOptionDef* optdef = config_def->get(key);
        if (!optdef) continue;

        // Auto-init: if this scalar key is missing from VO, create entry
        // with all slots = NaN (no override). This ensures we don't
        // accidentally clobber reset/parent state for other extruders.
        if (!has(key)) {
            double nan = std::numeric_limits<double>::quiet_NaN();
            switch (optdef->type) {
            case coFloat:
                floats[key].assign(variant_count, nan);
                break;
            case coFloatOrPercent: {
                floats[key].assign(variant_count, nan);
                strings[key].assign(variant_count, std::string());
                break;
            }
            case coBool:
                floats[key].assign(variant_count, nan);
                break;
            case coInt:
                floats[key].assign(variant_count, nan);
                break;
            default: continue;
            }
        }

        // Apply VO[variant_index] → config scalar.
        if (!has_variant(key, variant_index)) {
            continue;
        }
        // For coFloatOrPercent, uses the raw string to preserve "50%" notation.
        switch (optdef->type) {
        case coFloat:
            static_cast<ConfigOptionFloat*>(opt)->value = get_float(key, variant_index);
            break;
        case coFloatOrPercent: {
            std::string raw = get_string(key, variant_index);
            if (!raw.empty()) {
                auto* fop = static_cast<ConfigOptionFloatOrPercent*>(opt);
                if (raw.back() == '%') {
                    fop->value = std::stod(raw.substr(0, raw.size() - 1));
                    fop->percent = true;
                } else {
                    fop->value = std::stod(raw);
                    fop->percent = false;
                }
            }
            break;
        }
        case coBool:
            static_cast<ConfigOptionBool*>(opt)->value = (get_float(key, variant_index) != 0.0);
            break;
        case coInt:
            static_cast<ConfigOptionInt*>(opt)->value = (int)get_float(key, variant_index);
            break;
        default: break;
        }
    }
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::save_from_config
// ────────────────────────────────────────────────────────────────
// Reads the current scalar config values and writes them to VO[variant_index].
// This preserves user edits when switching between Left/Right nozzle tabs:
//   1. User edits inner_wall_speed on Left tab
//   2. save_from_config() stores the edit in VO[left_variant_index]
//   3. apply_to_config() loads Right variant values
//   4. User can switch back to Left and find their edit preserved
//
// Same auto-init logic as apply_to_config for keys with no VO entry.
// Called by DynamicPrintConfig::save_variant_overrides() on tab switch.
void VariantOverrides::save_from_config(const DynamicPrintConfig& config, int variant_index,
                                         const std::set<std::string>& keys, bool force)
{
    const ConfigDef* config_def = config.def();
    if (!config_def) return;

    int variant_count = determine_variant_count(config, *this);
    if (variant_count <= 0) return;

    for (const auto& key : keys) {
        const ConfigOption* opt = config.option(key);
        if (!opt) continue;
        const ConfigOptionDef* optdef = config_def->get(key);
        if (!optdef) continue;

        // Auto-init: create VO entry for this key.
        // ONLY set the active variant_index slot; all other slots = NaN
        // (meaning "no override, use parent value").
        // This prevents a per-object edit on one extruder from clobbering
        // the reset/parent state of the other extruder's slot.
        if (!has(key) && variant_count > 0) {
            double nan = std::numeric_limits<double>::quiet_NaN();
            switch (optdef->type) {
            case coFloat:
                floats[key].assign(variant_count, nan);
                break;
            case coFloatOrPercent: {
                floats[key].assign(variant_count, nan);
                strings[key].assign(variant_count, std::string());
                break;
            }
            case coBool:
                floats[key].assign(variant_count, nan);
                break;
            case coInt:
                floats[key].assign(variant_count, nan);
                break;
            default: continue;
            }
        }

        if (!has(key)) continue;

        // Skip saving to reset (NaN) slots — preserves the "reset" state.
        // But when force=true (explicit user edit), overwrite the NaN slot.
        if (!force && !has_variant(key, variant_index)) {
            continue;
        }

        // Config scalar → VO[variant_index].
        // For coFloatOrPercent, also saves the serialized string to preserve % notation.
        switch (optdef->type) {
        case coFloat:
            set_float(key, variant_index, static_cast<const ConfigOptionFloat*>(opt)->value);
            break;
        case coFloatOrPercent: {
            const auto* fop = static_cast<const ConfigOptionFloatOrPercent*>(opt);
            set_float(key, variant_index, fop->value);
            set_string(key, variant_index, fop->serialize());
            break;
        }
        case coBool:
            set_float(key, variant_index, static_cast<const ConfigOptionBool*>(opt)->value ? 1.0 : 0.0);
            break;
        case coInt:
            set_float(key, variant_index, (double)static_cast<const ConfigOptionInt*>(opt)->value);
            break;
        default: break;
        }
    }
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::expand_to_vectors
// ────────────────────────────────────────────────────────────────
// Converts scalar config + VO arrays into vector ConfigOptions so that
// save_to_json() produces BBS-compatible arrays:
//   "inner_wall_speed": [300, 600, 300, 600]
//
// For each key in VO that also exists in print_options_with_variant:
//   1. Read the VO array (all variant values)
//   2. Replace the scalar ConfigOption with a vector ConfigOption
// After expansion, clears VO (now redundant — values in config vectors).
//
// Called by DynamicPrintConfig::expand_variant_overrides_to_vectors()
// before JSON serialization.
void VariantOverrides::expand_to_vectors(DynamicPrintConfig& config)
{
    if (empty()) return;

    std::vector<std::string> keys_to_expand;
    for (const auto& [key, vals] : floats) {
        if (vals.empty() || !config.has(key) || print_options_with_variant.count(key) == 0)
            continue;
        keys_to_expand.push_back(key);
    }

    for (const auto& key : keys_to_expand) {
        const auto& float_vals = floats.at(key);
        int vc = (int)float_vals.size();
        if (vc <= 1) continue;

        const ConfigOptionDef* optdef = print_config_def.get(key);
        if (!optdef) continue;

        bool has_str = strings.count(key) > 0 && strings.at(key).size() == (size_t)vc;

        switch (optdef->type) {
        case coFloat: {
            // Use Nullable variant so NaN slots serialize as "nil"
            auto* v = new ConfigOptionFloatsNullable();
            v->values.assign(float_vals.begin(), float_vals.end());
            config.set_key_value(key, v);
            break;
        }
        case coFloatOrPercent: {
            if (has_str) {
                // Use Nullable variant so NaN slots serialize as "nil"
                auto* v = new ConfigOptionFloatsOrPercentsNullable();
                v->values.resize(vc);
                for (int i = 0; i < vc; ++i) {
                    const std::string& s = strings.at(key)[i];
                    v->values[i] = FloatOrPercent{float_vals[i], !s.empty() && s.back() == '%'};
                }
                config.set_key_value(key, v);
            } else {
                auto* v = new ConfigOptionFloatsNullable();
                v->values.assign(float_vals.begin(), float_vals.end());
                config.set_key_value(key, v);
            }
            break;
        }
        case coBool: {
            // Use Nullable variant so NaN slots serialize as "nil"
            auto* v = new ConfigOptionBoolsNullable();
            v->values.resize(vc);
            for (int i = 0; i < vc; ++i) {
                if (std::isnan(float_vals[i]))
                    v->values[i] = (unsigned char)2; // nil sentinel for bools
                else
                    v->values[i] = (float_vals[i] != 0.0) ? 1 : 0;
            }
            config.set_key_value(key, v);
            break;
        }
        case coInt: {
            // Use Nullable variant so NaN slots serialize as "nil"
            auto* v = new ConfigOptionIntsNullable();
            v->values.resize(vc);
            for (int i = 0; i < vc; ++i) {
                if (std::isnan(float_vals[i]))
                    v->values[i] = std::numeric_limits<int>::max(); // nil sentinel for ints
                else
                    v->values[i] = (int)float_vals[i];
            }
            config.set_key_value(key, v);
            break;
        }
        default: break;
        }
    }
    clear();
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::compress_from_vectors
// ────────────────────────────────────────────────────────────────
// Inverse of expand_to_vectors(). After loading a BBS-style JSON preset
// that contains array values like:
//   "inner_wall_speed": [300, 600, 300, 600]
//
// This method:
//   1. Reads the vector ConfigOption (ConfigOptionFloats, etc.)
//   2. Stores all values in this VO
//   3. Replaces the vector with a scalar = values[active_variant_index]
//
// OrcaSlicer UI works with scalars, so after compression the config
// holds the active variant's value while VO preserves all variants.
//
// Called by DynamicPrintConfig::compress_vectors_to_variant_overrides()
// after JSON load.
void VariantOverrides::compress_from_vectors(DynamicPrintConfig& config, int active_variant_index)
{
    for (const auto& key : print_options_with_variant) {
        ConfigOption* opt = config.option(key, false);
        if (!opt || !opt->is_vector()) continue;

        int vec_size = (int)static_cast<ConfigOptionVectorBase*>(opt)->size();
        if (vec_size <= 1) continue;

        const ConfigOptionDef* optdef = print_config_def.get(key);
        if (!optdef) continue;

        int idx = std::min(active_variant_index, vec_size - 1);

        switch (optdef->type) {
        case coFloat: {
            // Handle both Nullable and non-Nullable (from legacy saves)
            if (const auto* vn = dynamic_cast<ConfigOptionFloatsNullable*>(opt)) {
                floats[key] = vn->values;  // NaN values preserved
                double scalar = vn->is_nil(idx) ? 0.0 : vn->values[idx];
                config.set_key_value(key, new ConfigOptionFloat(scalar));
            } else if (const auto* v = dynamic_cast<ConfigOptionFloats*>(opt)) {
                floats[key] = v->values;
                config.set_key_value(key, new ConfigOptionFloat(v->values[idx]));
            }
            break;
        }
        case coFloatOrPercent: {
            if (const auto* fvn = dynamic_cast<ConfigOptionFloatsOrPercentsNullable*>(opt)) {
                floats[key].resize(vec_size);
                strings[key].resize(vec_size);
                for (int i = 0; i < vec_size; ++i) {
                    floats[key][i] = fvn->values[i].value;  // NaN preserved
                    if (std::isnan(fvn->values[i].value))
                        strings[key][i].clear();
                    else
                        strings[key][i] = fvn->values[i].percent ?
                            (std::to_string((int)fvn->values[i].value) + "%") :
                            std::to_string(fvn->values[i].value);
                }
                double val = fvn->is_nil(idx) ? 0.0 : fvn->values[idx].value;
                bool pct = fvn->is_nil(idx) ? false : fvn->values[idx].percent;
                config.set_key_value(key, new ConfigOptionFloatOrPercent(val, pct));
            } else if (const auto* fv = dynamic_cast<ConfigOptionFloatsOrPercents*>(opt)) {
                floats[key].resize(vec_size);
                strings[key].resize(vec_size);
                for (int i = 0; i < vec_size; ++i) {
                    floats[key][i] = fv->values[i].value;
                    strings[key][i] = fv->values[i].percent ?
                        (std::to_string((int)fv->values[i].value) + "%") :
                        std::to_string(fv->values[i].value);
                }
                config.set_key_value(key, new ConfigOptionFloatOrPercent(
                    fv->values[idx].value, fv->values[idx].percent));
            } else if (const auto* f = dynamic_cast<ConfigOptionFloatsNullable*>(opt)) {
                floats[key] = f->values;
                double val = f->is_nil(idx) ? 0.0 : f->values[idx];
                config.set_key_value(key, new ConfigOptionFloatOrPercent(val, false));
            } else if (const auto* f = dynamic_cast<ConfigOptionFloats*>(opt)) {
                floats[key] = f->values;
                config.set_key_value(key, new ConfigOptionFloatOrPercent(f->values[idx], false));
            }
            break;
        }
        case coBool: {
            // Handle both Nullable and non-Nullable
            if (const auto* vn = dynamic_cast<ConfigOptionBoolsNullable*>(opt)) {
                floats[key].resize(vec_size);
                for (int i = 0; i < vec_size; ++i) {
                    floats[key][i] = vn->is_nil(i) ?
                        std::numeric_limits<double>::quiet_NaN() :
                        (vn->values[i] ? 1.0 : 0.0);
                }
                bool scalar = vn->is_nil(idx) ? false : (bool)vn->values[idx];
                config.set_key_value(key, new ConfigOptionBool(scalar));
            } else if (const auto* v = dynamic_cast<ConfigOptionBools*>(opt)) {
                floats[key].resize(vec_size);
                for (int i = 0; i < vec_size; ++i) floats[key][i] = v->values[i] ? 1.0 : 0.0;
                config.set_key_value(key, new ConfigOptionBool(v->values[idx]));
            }
            break;
        }
        case coInt: {
            // Handle both Nullable and non-Nullable
            if (const auto* vn = dynamic_cast<ConfigOptionIntsNullable*>(opt)) {
                floats[key].resize(vec_size);
                for (int i = 0; i < vec_size; ++i) {
                    floats[key][i] = vn->is_nil(i) ?
                        std::numeric_limits<double>::quiet_NaN() :
                        (double)vn->values[i];
                }
                int scalar = vn->is_nil(idx) ? 0 : vn->values[idx];
                config.set_key_value(key, new ConfigOptionInt(scalar));
            } else if (const auto* v = dynamic_cast<ConfigOptionInts*>(opt)) {
                floats[key].resize(vec_size);
                for (int i = 0; i < vec_size; ++i) floats[key][i] = (double)v->values[i];
                config.set_key_value(key, new ConfigOptionInt(v->values[idx]));
            }
            break;
        }
        default: break;
        }
    }
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::parse_variant_csv
// ────────────────────────────────────────────────────────────────
// BBS 3MF files store per-variant values as comma-separated strings:
//   <config key="inner_wall_speed" value="300,600,300,600" />
//
// This method parses such CSV strings into a vector of doubles.
// Returns nullopt if the key is not variant-aware or value is not CSV.
// Only returns a result for multi-value CSV (2+ values).
std::optional<std::vector<double>> VariantOverrides::parse_variant_csv(
    const std::string& key, const std::string& value)
{
    if (print_options_with_variant.count(key) == 0)
        return std::nullopt;
    if (value.find(',') == std::string::npos)
        return std::nullopt;

    std::vector<double> vals;
    std::istringstream iss(value);
    std::string token;
    while (std::getline(iss, token, ',')) {
        boost::trim(token);
        if (!token.empty()) {
            if (token == "nil") {
                vals.push_back(std::numeric_limits<double>::quiet_NaN());
            } else {
                try { vals.push_back(std::stod(token)); }
                catch (...) { return std::nullopt; }
            }
        }
    }
    return vals.size() > 1 ? std::optional(vals) : std::nullopt;
}

// ────────────────────────────────────────────────────────────────
// ────────────────────────────────────────────────────────────────
// VariantOverrides::swap_extruder_order  [DEPRECATED — no longer called]
// ────────────────────────────────────────────────────────────────
// Rotates all VO arrays by n/2 to swap extruder halves.
// Previously needed when internal order differed from BBS file order.
// After Tab.cpp L/R fix, internal order == BBS order, so no swap needed.
// Kept for reference; can be removed in a future cleanup.
void VariantOverrides::swap_extruder_order()
{
    for (auto& [key, vals] : floats) {
        if (vals.size() > 1 && print_options_with_variant.count(key))
            std::rotate(vals.begin(), vals.begin() + (vals.size() / 2), vals.end());
    }
    for (auto& [key, vals] : strings) {
        if (vals.size() > 1 && print_options_with_variant.count(key))
            std::rotate(vals.begin(), vals.begin() + (vals.size() / 2), vals.end());
    }
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::prepare_for_3mf_save
// ────────────────────────────────────────────────────────────────
// Prepare a DynamicPrintConfig for 3MF serialization:
//   Expand VO into vector ConfigOptions for serialization.
//   Internal order now matches BBS file order (Left first)
//   so no extruder swap is needed.
// Caller should pass a COPY of the config (this modifies in-place).
void VariantOverrides::prepare_for_3mf_save(DynamicPrintConfig& config)
{
    if (config.variant_overrides().empty())
        return;
    // No swap needed: after Tab.cpp fix, internal order == BBS file order.
    config.expand_variant_overrides_to_vectors();
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::load_from_3mf_compress
// ────────────────────────────────────────────────────────────────
// After loading a BBS-style JSON from 3MF:
//   Compress vector ConfigOptions into VO.
//   No swap needed: BBS file order == internal order (Left first).
void VariantOverrides::load_from_3mf_compress(DynamicPrintConfig& config, int active_variant_index)
{
    config.compress_vectors_to_variant_overrides(active_variant_index);
    // No swap needed: internal order matches BBS file order.
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::is_variant_csv
// ────────────────────────────────────────────────────────────────
// Returns true if a 3MF metadata key/value pair is a variant-aware CSV
// that should be SKIPPED in the first-pass set_deserialize().
bool VariantOverrides::is_variant_csv(const std::string& key, const std::string& value)
{
    return print_options_with_variant.count(key) > 0 &&
           value.find(',') != std::string::npos;
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::try_load_per_object_3mf_metadata
// ────────────────────────────────────────────────────────────────
// Parse a per-object variant-aware CSV from 3MF metadata and store
// in config's VO. Handles:
//   1. CSV parsing (with "nil" → NaN)
//   2. BBS→internal extruder order swap
//   3. Store in VO floats + set scalar config value
void VariantOverrides::try_load_per_object_3mf_metadata(
    const std::string& key, const std::string& value,
    DynamicPrintConfig& config)
{
    auto parsed = parse_variant_csv(key, value);
    if (!parsed || parsed->size() <= 1)
        return;

    auto& vals = *parsed;
    // No swap needed: BBS file order matches internal order (Left first).

    config.variant_overrides().floats[key] = vals;

    // Set scalar to first non-NaN value, or 0 if all NaN
    double scalar = 0.0;
    for (double v : vals) {
        if (!std::isnan(v)) { scalar = v; break; }
    }
    config.set_key_value(key, new ConfigOptionFloat(scalar));
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::precompute_overlays
// ────────────────────────────────────────────────────────────────
// Called once at print start by GCode::precompute_extruder_speed_overrides().
// Builds ALL per-extruder overlay configs in a single pass:
//
//   1. For each physical extruder, compute its variant_index
//   2. Build a global overlay from the global VO
//   3. For each object with its own VO, build per-object overlays
//      merged on top of the global overlay (per-object takes priority)
//
// Result is stored in GCode::VariantAwareConfig and applied at
// each toolchange during G-code generation.
//
// This decouples VO resolution from the hot path of G-code writing.
PrecomputedOverlays VariantOverrides::precompute_overlays(
    const VariantOverrides& global_vo,
    const ConfigBase&       full_config,
    unsigned int            num_extruders,
    const std::vector<std::pair<size_t, const VariantOverrides*>>& object_vos)
{
    PrecomputedOverlays result;
    if (global_vo.empty() && object_vos.empty())
        return result;

    if (!global_vo.empty()) {
        for (unsigned int eid = 0; eid < num_extruders; ++eid) {
            int vi = compute_variant_index(eid, full_config);
            if (vi < 0) continue;
            auto overlay = global_vo.build_overlay(vi);
            if (!overlay.empty()) {
                result.extruder_overrides[eid] = std::move(overlay);
            }
        }
    }

    for (const auto& [obj_id, obj_vo_ptr] : object_vos) {
        if (!obj_vo_ptr || obj_vo_ptr->empty()) continue;
        for (unsigned int eid = 0; eid < num_extruders; ++eid) {
            int vi = compute_variant_index(eid, full_config);
            if (vi < 0) continue;
            const DynamicPrintConfig* base = nullptr;
            if (const auto it = result.extruder_overrides.find(eid); it != result.extruder_overrides.end())
                base = &it->second;
            auto overlay = obj_vo_ptr->build_overlay(vi, base);
            if (!overlay.empty())
                result.object_extruder_overrides[obj_id][eid] = std::move(overlay);
        }
    }
    return result;
}

// ────────────────────────────────────────────────────────────────
// VariantOverrides::dump
// ────────────────────────────────────────────────────────────────
// Debug utility: logs all VO contents via BOOST_LOG_TRIVIAL(info).
// Each key outputs as: "prefix key: [val0, val1, ...]"
// For coFloatOrPercent keys with string overrides, uses the raw
// string ("50%") instead of the numeric value.
void VariantOverrides::dump(const std::string& prefix) const
{
    for (const auto& [key, vals] : floats) {
        std::string line = prefix + " " + key + ": [";
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i > 0) line += ", ";
            const auto sit = strings.find(key);
            if (sit != strings.end() && i < sit->second.size() && !sit->second[i].empty())
                line += sit->second[i];
            else
                line += std::to_string(vals[i]);
        }
        line += "]";
        BOOST_LOG_TRIVIAL(info) << line;
    }
}

} // namespace Slic3r
