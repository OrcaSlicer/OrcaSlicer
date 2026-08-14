#include "PublishSettings.hpp"

#include "PresetBundle.hpp"
#include "Preset.hpp"
#include "PrintConfig.hpp"

#include <algorithm>

namespace Slic3r {

const std::set<std::string>& publish_structural_keys()
{
    // Structural / non-publishable keys. The *_settings_id keys are also part of
    // PresetCollection::skipped_in_dirty (Preset.cpp) and are excluded there too.
    // This mirrors the structural keys stripped from configs in Preset.cpp
    // (profile_print_params_same) plus other keys that must never be published
    // because they would rewrite the user's preset inheritance/structure.
    static const std::set<std::string> structural_keys = {
        "printer_settings_id", "filament_settings_id", "print_settings_id",
        "sla_print_settings_id", "sla_material_settings_id",
        "compatible_printers", "compatible_prints",
        "compatible_printers_condition", "compatible_prints_condition",
        "default_filament_profile", "default_print_profile",
        "default_sla_print_profile", "default_sla_material_profile",
        "extruder_count", "bed_shape",
        "inherits", "inherits_group",
        "printer_technology", "printer_model", "printer_variant",
        "physical_printer_settings_id", "filament_ids",
        "different_settings_to_system"
    };
    return structural_keys;
}

// The printer tab's "Retraction" optgroup (TabPrinter::build_fff, Tab.cpp), in tab order.
// KEEP IN SYNC with that optgroup: the published-3MF printer allowlist is built from these
// lists, so any key shown there must be publishable here (and vice versa).
const std::vector<PublishablePrinterOption>& publishable_printer_retraction_options()
{
    static const std::vector<PublishablePrinterOption> options = {
        { "retraction_length",              "printer_extruder_retraction#length" },
        { "retract_restart_extra",          "printer_extruder_retraction#extra-length-on-restart" },
        { "retraction_speed",               "printer_extruder_retraction#retraction-speed" },
        { "deretraction_speed",             "printer_extruder_retraction#deretraction-speed" },
        { "retraction_minimum_travel",      "printer_extruder_retraction#travel-distance-threshold" },
        { "retract_when_changing_layer",    "printer_extruder_retraction#retract-on-layer-change" },
        { "wipe",                           "printer_extruder_retraction#wipe-while-retracting" },
        { "wipe_distance",                  "printer_extruder_retraction#wipe-distance" },
        { "retract_before_wipe",            "printer_extruder_retraction#retract-amount-before-wipe" },
        { "retract_after_wipe",             "printer_extruder_retraction#retract-amount-after-wipe" },
    };
    return options;
}

// The printer tab's "Z-Hop" optgroup (TabPrinter::build_fff, Tab.cpp), in tab order. KEEP IN
// SYNC with that optgroup, same as publishable_printer_retraction_options().
const std::vector<PublishablePrinterOption>& publishable_printer_z_hop_options()
{
    static const std::vector<PublishablePrinterOption> options = {
        { "retract_lift_enforce", "printer_extruder_z_hop#on-surfaces" },
        { "z_hop_types",          "printer_extruder_z_hop#z-hop-type" },
        { "z_hop",                "printer_extruder_z_hop#z-hop-height" },
        { "travel_slope",         "printer_extruder_z_hop#traveling-angle" },
        { "retract_lift_above",   "printer_extruder_z_hop#only-lift-z-above" },
        { "retract_lift_below",   "printer_extruder_z_hop#only-lift-z-below" },
    };
    return options;
}

const std::set<std::string>& publishable_printer_keys()
{
    // The union of the printer tab's "Retraction" and "Z-Hop" optgroups. The "Retraction when
    // switching material" keys are intentionally excluded: toolchange retraction is
    // device/profile territory, not a publishable behavior tweak.
    static const std::set<std::string> printer_keys = [] {
        std::set<std::string> keys;
        for (const PublishablePrinterOption &opt : publishable_printer_retraction_options())
            keys.insert(opt.key);
        for (const PublishablePrinterOption &opt : publishable_printer_z_hop_options())
            keys.insert(opt.key);
        return keys;
    }();
    return printer_keys;
}

std::vector<std::string> collect_dirty_settings_keys(const PresetBundle& bundle)
{
    std::vector<std::string> keys;

    auto append_dirty = [&keys](const std::vector<std::string>& dirty) {
        for (const std::string& key : dirty) {
            if (std::find(keys.begin(), keys.end(), key) == keys.end())
                keys.push_back(key);
        }
    };

    // Print and printer presets each track a single edited preset; filaments may span
    // multiple slots (multi-material). Union the dirty keys of each collection's edited
    // preset; this feeds only the Publish dialog's pre-check.
    append_dirty(bundle.prints.current_dirty_options(true));
    append_dirty(bundle.printers.current_dirty_options(true));
    append_dirty(bundle.filaments.current_dirty_options(true));

    return keys;
}

DynamicPrintConfig filter_published_config(
    const DynamicPrintConfig &full_config,
    const std::vector<std::string> &published_keys,
    const std::vector<PublishedMaterialEntry> &material_keys)
{
    DynamicPrintConfig filtered;

    std::set<std::string> base_keys_to_include;

    // 1. Mandatory material identity & slot count keys for 3MF validation/normalization
    static const std::vector<std::string> s_material_identity_keys = {
        "filament_colour",
        "filament_type",
        "filament_vendor",
        "filament_ids",
        "filament_diameter",
        "filament_self_index",
        "filament_extruder_variant"
    };
    for (const std::string &key : s_material_identity_keys)
        base_keys_to_include.insert(key);

    // 2. Published plate / bed geometry keys (wipe tower positioning)
    static const std::vector<std::string> s_plate_geometry_keys = {
        "wipe_tower_x",
        "wipe_tower_y",
        "wipe_tower_rotation_angle"
    };
    for (const std::string &key : s_plate_geometry_keys)
        base_keys_to_include.insert(key);

    // 3. Process and printer published keys
    for (const std::string &key : published_keys) {
        const std::string base_key = key.substr(0, key.find('#'));
        if (!base_key.empty())
            base_keys_to_include.insert(base_key);
    }

    // 4. Material-specific published keys
    for (const PublishedMaterialEntry &entry : material_keys) {
        for (const std::string &key : entry.keys) {
            const std::string base_key = key.substr(0, key.find('#'));
            if (!base_key.empty())
                base_keys_to_include.insert(base_key);
        }
    }

    // Copy selected options from full_config into filtered config
    for (const std::string &key : base_keys_to_include) {
        if (const ConfigOption *opt = full_config.option(key))
            filtered.set_key_value(key, opt->clone());
    }

    return filtered;
}

} // namespace Slic3r
