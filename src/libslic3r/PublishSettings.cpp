#include "PublishSettings.hpp"

#include "PresetBundle.hpp"
#include "Preset.hpp"
#include "PrintConfig.hpp"
#include "MaterialType.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace Slic3r {

std::string normalize_filament_type(const std::string& type)
{
    if (type.empty())
        return type;
    if (MaterialType::find(type) != nullptr)
        return type;
    // "PLA High Speed" -> "PLA": strip a space-separated modifier, but keep dash-separated
    // types like "PA-CF" / "PETG-CF" intact (they are distinct materials, not modifiers).
    const size_t sep = type.find(' ');
    if (sep != std::string::npos) {
        const std::string base = type.substr(0, sep);
        if (MaterialType::find(base) != nullptr)
            return base;
    }
    return type;
}

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
    // Base keys that must never be masked: identity, plate geometry, process/printer keys and
    // partially-published material keys keep today's whole-vector serialization (all slots).
    std::set<std::string>         mask_exempt_keys;
    // For keys carried only by "full" entries: base key -> author slots whose values must
    // survive; the other slots are masked to their defaults so a full publish does not leak
    // the author's unrelated slot data.
    std::map<std::string, std::set<int>> full_slot_map;

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
    for (const std::string &key : s_material_identity_keys) {
        base_keys_to_include.insert(key);
        mask_exempt_keys.insert(key);
    }

    // 2. Published plate / bed geometry keys (wipe tower positioning)
    static const std::vector<std::string> s_plate_geometry_keys = {
        "wipe_tower_x",
        "wipe_tower_y",
        "wipe_tower_rotation_angle"
    };
    for (const std::string &key : s_plate_geometry_keys) {
        base_keys_to_include.insert(key);
        mask_exempt_keys.insert(key);
    }

    // 3. Process and printer published keys
    for (const std::string &key : published_keys) {
        const std::string base_key = key.substr(0, key.find('#'));
        if (!base_key.empty()) {
            base_keys_to_include.insert(base_key);
            mask_exempt_keys.insert(base_key);
        }
    }

    // 4. Material-specific published keys
    for (const PublishedMaterialEntry &entry : material_keys) {
        for (const std::string &key : entry.keys) {
            const std::string base_key = key.substr(0, key.find('#'));
            if (!base_key.empty()) {
                base_keys_to_include.insert(base_key);
                mask_exempt_keys.insert(base_key);
            }
        }
        // 4b. "Full publish" entries carry the entire slot; the values of the covered keys are
        // masked to the author's slot on export (see the copy loop below).
        for (const std::string &key : entry.full_keys) {
            const std::string base_key = key.substr(0, key.find('#'));
            if (base_key.empty())
                continue;
            base_keys_to_include.insert(base_key);
            if (entry.slot >= 0)
                full_slot_map[base_key].insert(entry.slot);
        }
    }

    // Mask a vector option's slots that are not author-published: copy the option default over
    // each non-published index. Keys without an option default are left unmasked (the file then
    // carries the whole vector, matching the partial-publish behavior).
    auto mask_slots = [](ConfigOption &opt, const ConfigOptionDef *def, const std::set<int> &keep_slots) {
        auto *vec = dynamic_cast<ConfigOptionVectorBase*>(&opt);
        if (vec == nullptr || vec->size() == 0 || def == nullptr || !def->default_value)
            return;
        if (def->default_value->type() != opt.type())
            return;
        const auto *default_vec = dynamic_cast<const ConfigOptionVectorBase*>(def->default_value.get());
        if (default_vec == nullptr || default_vec->empty())
            return;
        for (size_t idx = 0; idx < vec->size(); ++idx)
            if (keep_slots.count(static_cast<int>(idx)) == 0)
                vec->set_at(def->default_value.get(), idx, 0);
    };

    // Copy selected options from full_config into filtered config
    for (const std::string &key : base_keys_to_include) {
        if (const ConfigOption *opt = full_config.option(key)) {
            ConfigOption *cloned = opt->clone();
            if (mask_exempt_keys.count(key) == 0) {
                const auto it = full_slot_map.find(key);
                if (it != full_slot_map.end() && !it->second.empty())
                    mask_slots(*cloned, print_config_def.get(key), it->second);
            }
            filtered.set_key_value(key, cloned);
        }
    }

    return filtered;
}

} // namespace Slic3r
