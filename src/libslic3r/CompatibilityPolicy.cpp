#include "CompatibilityPolicy.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "Config.hpp"
#include "libslic3r.h"
#include "Preset.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

namespace Slic3r {
namespace CompatibilityPolicy {

namespace {

// Bookkeeping keys that are never carried into the payload, even if they would
// otherwise classify as process / filament options. They describe the preset
// dependency graph, not a printable value, and must come from the user's own
// presets.
const std::set<std::string>& bookkeeping_denylist()
{
    static const std::set<std::string> keys = {
        "compatible_printers",
        "compatible_printers_condition",
        "compatible_prints",
        "compatible_prints_condition",
        "inherits",
        "print_settings_id",
        "printer_settings_id",
        "filament_settings_id",
        "enable_filament_dynamic_map",
    };
    return keys;
}

// Hard-gated filament keys that MUST stay gated when no filament slot matches the
// user's type: they are never carried into the payload (filter) nor applied
// (apply). These are the material's identity, its temperatures, its G-code, its
// retraction and flow caps, and printer-coupled plumbing. Everything else — flow
// ratio, pressure advance, prime/ramming, loading, ironing, etc. — transfers to
// all slots so the user keeps as much as possible on a type mismatch. When a slot
// does match, these keys are gated per-slot exactly like any other filament key.
const std::set<std::string>& hard_material_denylist()
{
    static const std::set<std::string> keys = {
        // Material identity.
        "filament_type", "filament_diameter", "pellet_flow_coefficient",
        "filament_density", "filament_soluble", "filament_is_support",
        "filament_printable", "filament_vendor", "filament_extruder_compatibility",
        "filament_shrink", "filament_shrinkage_compensation_z",
        // Drying (Dev / AMS).
        "filament_dev_ams_drying_ams_limitations", "filament_dev_ams_drying_temperature",
        "filament_dev_ams_drying_time", "filament_dev_ams_drying_heat_distortion_temperature",
        "filament_dev_chamber_drying_bed_temperature", "filament_dev_chamber_drying_time",
        "filament_dev_drying_softening_temperature", "filament_dev_drying_cooling_temperature",
        // Nozzle temperatures.
        "nozzle_temperature", "nozzle_temperature_initial_layer",
        "nozzle_temperature_range_low", "nozzle_temperature_range_high",
        // Build-plate temperatures (and their initial-layer variants).
        "cool_plate_temp", "textured_cool_plate_temp", "eng_plate_temp",
        "hot_plate_temp", "textured_plate_temp", "supertack_plate_temp",
        "cool_plate_temp_initial_layer", "textured_cool_plate_temp_initial_layer",
        "eng_plate_temp_initial_layer", "hot_plate_temp_initial_layer",
        "textured_plate_temp_initial_layer", "supertack_plate_temp_initial_layer",
        "temperature_vitrification",
        // Chamber.
        "chamber_temperature", "chamber_minimal_temperature",
        "activate_chamber_temp_control",
        // Flush / pre-cooling / idle temperatures.
        "filament_flush_temp", "filament_flush_temp_fast",
        "filament_pre_cooling_temperature", "filament_pre_cooling_temperature_nc",
        "filament_preheat_temperature_delta", "idle_temperature",
        "filament_tower_interface_print_temp",
        // Filament G-code.
        "filament_start_gcode", "filament_end_gcode",
        "filament_change_extrusion_role_gcode",
        // Retraction (kept gated for safety).
        "filament_retraction_length", "filament_retraction_speed",
        "filament_deretraction_speed", "filament_retract_before_wipe",
        "filament_retract_restart_extra", "filament_retract_lift_above",
        "filament_retract_lift_below", "filament_retract_when_changing_layer",
        "filament_retract_length_nc", "filament_z_hop", "filament_z_hop_types",
        "filament_retract_after_wipe", "filament_retract_lift_enforce",
        "filament_retraction_minimum_travel", "filament_retract_length_toolchange",
        "filament_retract_restart_extra_toolchange",
        "filament_retraction_distances_when_cut", "filament_long_retractions_when_cut",
        // Wipe (kept gated for safety).
        "filament_wipe", "filament_wipe_distance",
        // Flow caps / tower / change (kept gated for safety).
        "filament_change_length", "filament_flush_volumetric_speed",
        "filament_cooling_before_tower",
        // Flow caps (kept gated for safety).
        "filament_max_volumetric_speed", "filament_adaptive_volumetric_speed",
        "volumetric_speed_coefficients",
        // Printer-coupled (must drop, not transfer).
        "filament_extruder_variant",
    };
    return keys;
}

// Process keys that reference filament slots. They must come from the user's own
// matched filament, so they are never carried into the payload. The suffix rules
// cover the legacy `*_extruder` keys (renamed to `*_filament_id` by handle_legacy)
// and the modern `*_filament_id` keys.
const std::set<std::string>& cross_reference_denylist()
{
    static const std::set<std::string> keys = {
        "support_filament",
        "support_interface_filament",
        "default_filament_profile",
    };
    return keys;
}

bool ends_with(const std::string& str, const std::string& suffix)
{
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_printer_key(const std::string& key)
{
    const std::vector<std::string>& printer = Preset::printer_options();
    if (std::find(printer.begin(), printer.end(), key) != printer.end())
        return true;
    return printer_options_with_variant_1.count(key) != 0 ||
           printer_options_with_variant_2.count(key) != 0;
}

bool is_process_key(const std::string& key)
{
    const std::vector<std::string>& print = Preset::print_options();
    if (std::find(print.begin(), print.end(), key) != print.end())
        return true;
    return print_options_with_variant.count(key) != 0;
}

bool is_filament_key(const std::string& key)
{
    const std::vector<std::string>& filament = Preset::filament_options();
    if (std::find(filament.begin(), filament.end(), key) != filament.end())
        return true;
    return filament_options_with_variant.count(key) != 0;
}

// Match filament slots of `project` against `user_current` element-wise by
// filament_type. A slot matches only when both sides carry the same non-empty
// type. If filament_type is absent from either side, no slot matches (filament
// is gated off entirely).
std::vector<size_t> match_filament_slots(const DynamicPrintConfig& project, const DynamicPrintConfig& user_current)
{
    std::vector<size_t> matched;
    const ConfigOptionStrings* project_types = project.option<ConfigOptionStrings>("filament_type");
    const ConfigOptionStrings* user_types    = user_current.option<ConfigOptionStrings>("filament_type");
    if (project_types == nullptr || user_types == nullptr)
        return matched;
    const size_t n = std::min(project_types->values.size(), user_types->values.size());
    for (size_t i = 0; i < n; ++i)
        if (!project_types->values[i].empty() && project_types->values[i] == user_types->values[i])
            matched.push_back(i);
    return matched;
}

// Number of extruders in the user's current config, used to size the arrays that
// are written into the project_config layer.
size_t user_extruder_count(const DynamicPrintConfig& user_current)
{
    if (const ConfigOptionFloats* nd = user_current.option<ConfigOptionFloats>("nozzle_diameter"))
        if (!nd->values.empty())
            return nd->values.size();
    if (const ConfigOptionStrings* ft = user_current.option<ConfigOptionStrings>("filament_type"))
        if (!ft->values.empty())
            return ft->values.size();
    return 1;
}

// Build a per-slot copy of a filament array option where only the slots in
// `matched` keep the source value. Non-matched slots are set to nil when the
// option is nullable, otherwise they are overwritten with the user's own current
// value so that applying the payload is a no-op for those slots.
ConfigOption* gated_filament_option(const ConfigOption* src, const ConfigOption* user, const std::set<size_t>& matched)
{
    ConfigOption* result = src->clone();
    ConfigOptionVectorBase* vec = dynamic_cast<ConfigOptionVectorBase*>(result);
    if (vec == nullptr)
        return result; // scalar filament option: carry as-is

    const size_t n = vec->size();
    const ConfigOptionVectorBase* user_vec = dynamic_cast<const ConfigOptionVectorBase*>(user);
    for (size_t i = 0; i < n; ++i) {
        if (matched.count(i) != 0)
            continue;
        if (result->nullable()) {
            vec->set_at_to_nil(i);
        } else if (user_vec != nullptr && user_vec->type() == vec->type() && i < user_vec->size()) {
            vec->set_at(user_vec, i, i);
        }
    }
    return result;
}

// Build the array to write into the project_config layer for a filament option.
// The array is sized to `size` (the user's extruder count). Slots that are both
// matched and present in the payload keep the payload value; every other slot
// keeps the user's own current value (nil for nullable options, so the value is
// inherited from the user's preset).
ConfigOption* build_gated_array(const ConfigOption* src, const ConfigOption* user, size_t size, const std::set<size_t>& matched)
{
    ConfigOption* result = src->clone();
    ConfigOptionVectorBase* vec = dynamic_cast<ConfigOptionVectorBase*>(result);
    if (vec == nullptr)
        return result;

    const size_t src_size = vec->size();
    vec->resize(size);

    const ConfigOptionVectorBase* user_vec = dynamic_cast<const ConfigOptionVectorBase*>(user);
    for (size_t i = 0; i < size; ++i) {
        const bool have_payload = i < src_size;
        if (have_payload && matched.count(i) != 0)
            continue; // matched slot with a payload value: keep it
        // Gated-off slot (or no payload value for this slot): do not override the
        // user's own setting.
        if (result->nullable()) {
            vec->set_at_to_nil(i);
        } else if (user_vec != nullptr && user_vec->type() == vec->type() && i < user_vec->size()) {
            vec->set_at(user_vec, i, i);
        }
    }
    return result;
}

} // namespace

FilterResult filter(const DynamicPrintConfig& full_config, const DynamicPrintConfig& user_current)
{
    FilterResult result;

    // Filament-type gating: which slots' types match the user's current config.
    result.filament_slots = match_filament_slots(full_config, user_current);
    const std::set<size_t> matched_set(result.filament_slots.begin(), result.filament_slots.end());
    const bool gate_filament = !result.filament_slots.empty();

    for (const std::string& key : full_config.keys()) {
        // 1. Never carry printer / machine keys.
        if (is_printer_key(key)) {
            result.dropped.push_back(key);
            continue;
        }
        // 2. Bookkeeping keys are dropped unconditionally.
        if (bookkeeping_denylist().count(key) != 0) {
            result.dropped.push_back(key);
            continue;
        }
        // 3. Cross-reference process keys referencing filament slots.
        if (cross_reference_denylist().count(key) != 0 ||
            ends_with(key, "_filament_id") || ends_with(key, "_extruder")) {
            result.dropped.push_back(key);
            continue;
        }
        // 4. Classify into process / filament scope.
        if (is_process_key(key)) {
            result.payload.set_key_value(key, full_config.option(key)->clone());
        } else if (is_filament_key(key)) {
            if (!gate_filament) {
                // No filament slot matched the user's config. Hard-gated
                // material keys are dropped; everything else is carried for all
                // slots so the user still gets the non-critical settings.
                if (hard_material_denylist().count(key) != 0) {
                    result.dropped.push_back(key);
                    continue;
                }
                const ConfigOptionVectorBase* vec =
                    dynamic_cast<const ConfigOptionVectorBase*>(full_config.option(key));
                std::set<size_t> all_slots;
                if (vec != nullptr)
                    for (size_t i = 0; i < vec->size(); ++i)
                        all_slots.insert(i);
                result.payload.set_key_value(key, gated_filament_option(full_config.option(key), user_current.option(key), all_slots));
                continue;
            }
            result.payload.set_key_value(key, gated_filament_option(full_config.option(key), user_current.option(key), matched_set));
        } else {
            // Unclassified / derived key: drop it (list hygiene).
            result.dropped.push_back(key);
        }
    }
    return result;
}

ApplyResult apply(DynamicPrintConfig& out, const DynamicPrintConfig& payload, const DynamicPrintConfig& user_current)
{
    ApplyResult result;

    size_t extruder_count = user_extruder_count(user_current);
    if (extruder_count == 0)
        extruder_count = 1;

    // Re-derive the matching filament slots against the opening user's current
    // config. This is authoritative regardless of where the payload came from.
    const std::vector<size_t> matched = match_filament_slots(payload, user_current);
    const std::set<size_t> matched_set(matched.begin(), matched.end());
    const bool gate_filament = !matched.empty();

    for (const std::string& key : payload.keys()) {
        // Defensive: never write printer / machine keys.
        if (is_printer_key(key)) {
            result.dropped.push_back(key);
            continue;
        }
        if (bookkeeping_denylist().count(key) != 0) {
            result.dropped.push_back(key);
            continue;
        }
        if (cross_reference_denylist().count(key) != 0 ||
            ends_with(key, "_filament_id") || ends_with(key, "_extruder")) {
            result.dropped.push_back(key);
            continue;
        }
        if (!is_process_key(key) && !is_filament_key(key)) {
            result.dropped.push_back(key);
            continue;
        }

        const ConfigOption* opt = payload.option(key);
        if (is_filament_key(key)) {
            if (!gate_filament) {
                // Filament gated off entirely. Material-critical keys cannot be
                // transferred across a type mismatch and are reported separately;
                // everything else is applied for all slots.
                if (hard_material_denylist().count(key) != 0) {
                    result.material_dropped.push_back(key);
                    continue;
                }
                std::set<size_t> all_slots;
                for (size_t i = 0; i < extruder_count; ++i)
                    all_slots.insert(i);
                out.set_key_value(key, build_gated_array(opt, user_current.option(key), extruder_count, all_slots));
                result.applied.push_back(key);
                continue;
            }
            out.set_key_value(key, build_gated_array(opt, user_current.option(key), extruder_count, matched_set));
            result.applied.push_back(key);
        } else {
            // Process key. Only genuinely per-extruder vectors (those in
            // print_options_with_variant) are resized to the user's extruder count;
            // non-per-extruder vectors (e.g. wipe_tower_x, flush_volumes_matrix,
            // gcode_substitutions) are applied as-is. When a per-extruder vector is
            // grown, the extra slots are filled with the user's own values rather than
            // defaults (0/empty).
            ConfigOption* applied = opt->clone();
            if (ConfigOptionVectorBase* vec = dynamic_cast<ConfigOptionVectorBase*>(applied)) {
                if (print_options_with_variant.count(key) != 0) {
                    const size_t src_size = vec->size();
                    const ConfigOptionVectorBase* user_vec = dynamic_cast<const ConfigOptionVectorBase*>(user_current.option(key));
                    vec->resize(extruder_count);
                    for (size_t i = src_size; i < extruder_count; ++i) {
                        if (applied->nullable()) {
                            vec->set_at_to_nil(i);
                        } else if (user_vec != nullptr && user_vec->type() == vec->type() && i < user_vec->size()) {
                            vec->set_at(user_vec, i, i);
                        }
                    }
                }
            }
            out.set_key_value(key, applied);
            result.applied.push_back(key);
        }
    }
    return result;
}

std::string serialize_payload(const DynamicPrintConfig& payload)
{
    // Persist via save_to_json to a temp file and read it back, mirroring how
    // embedded project presets are stored (bbs_3mf.cpp).
    boost::filesystem::path temp_file = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orcaslicer_compat_%%%%-%%%%-%%%%-%%%%");
    try {
        payload.save_to_json(temp_file.string(), "compatibility", "project", std::string(SLIC3R_VERSION));
        std::string json;
        load_string_file(temp_file, json);
        boost::system::error_code ec;
        boost::filesystem::remove(temp_file, ec);
        return json;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CompatibilityPolicy::serialize_payload: " << e.what();
        boost::system::error_code ec;
        boost::filesystem::remove(temp_file, ec);
        return std::string();
    }
}

DynamicPrintConfig deserialize_payload(const std::string& json, std::vector<std::string>& substituted)
{
    DynamicPrintConfig config;
    if (json.empty())
        return config;

    boost::filesystem::path temp_file = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orcaslicer_compat_%%%%-%%%%-%%%%-%%%%");
    try {
        save_string_file(temp_file, json);
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
        std::map<std::string, std::string> key_values;
        std::string reason;
        config.load_from_json(temp_file.string(), ctxt, false, key_values, reason);
        for (const ConfigSubstitution& sub : ctxt.substitutions)
            substituted.push_back(sub.opt_def ? sub.opt_def->opt_key : sub.old_value);
        for (const std::string& key : ctxt.unrecogized_keys)
            substituted.push_back(key);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CompatibilityPolicy::deserialize_payload: " << e.what();
    }
    boost::system::error_code ec;
    boost::filesystem::remove(temp_file, ec);
    return config;
}

} // namespace CompatibilityPolicy
} // namespace Slic3r
