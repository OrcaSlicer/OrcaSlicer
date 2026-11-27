#include "PublishSettings.hpp"

#include "Config.hpp"
#include "PresetBundle.hpp"
#include "Preset.hpp"
#include "PrintConfig.hpp"
#include "MaterialType.hpp"
#include "I18N.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Slic3r {

std::string publish_base_key(const std::string &key)
{
    const size_t pos = key.find('#');
    return pos == std::string::npos ? key : key.substr(0, pos);
}

// Parse the trailing "#N" index. Mirrors the importer's strict parse so the export side rejects
// the same variants a receiver would skip.
int publish_key_index(const std::string &key)
{
    const size_t pos = key.find('#');
    if (pos == std::string::npos || pos + 1 >= key.size())
        return -1;
    int idx = 0;
    for (size_t i = pos + 1; i < key.size(); ++i) {
        const char c = key[i];
        if (c < '0' || c > '9')
            return -1;
        idx = idx * 10 + (c - '0');
        if (idx > 1000000) // overflow guard; real vector sizes are tiny
            return -1;
    }
    return idx;
}

static const ConfigOptionStrings *filament_variant_list(const DynamicPrintConfig &config)
{
    return config.option<ConfigOptionStrings>("filament_extruder_variant");
}

std::vector<size_t> publish_filament_columns(const std::vector<int> &self_index, size_t slot)
{
    std::vector<size_t> columns;
    for (size_t column = 0; column < self_index.size(); ++column)
        if (self_index[column] == int(slot) + 1)
            columns.push_back(column);
    return columns;
}

std::vector<size_t> publish_filament_columns(const DynamicPrintConfig &uncompacted, size_t slot)
{
    const ConfigOptionInts *self_index = uncompacted.option<ConfigOptionInts>("filament_self_index");
    if (self_index != nullptr)
        return publish_filament_columns(self_index->values, slot);
    // Hand-built config without the layout: one variant per slot, as the legacy loader assumes.
    std::vector<size_t> columns;
    const ConfigOptionStrings *variants = filament_variant_list(uncompacted);
    if (variants != nullptr && slot < variants->size())
        columns.push_back(slot);
    return columns;
}

std::optional<size_t> publish_filament_local_to_file(const DynamicPrintConfig &uncompacted, size_t slot, size_t local)
{
    const std::vector<size_t> columns = publish_filament_columns(uncompacted, slot);
    if (local >= columns.size())
        return std::nullopt;
    return columns[local];
}

std::optional<size_t> publish_match_variant(const DynamicPrintConfig &config, std::optional<size_t> slot,
                                            std::string_view variant)
{
    const ConfigOptionStrings *variants = filament_variant_list(config);
    if (variants == nullptr)
        return std::nullopt;
    if (slot.has_value()) {
        for (const size_t column : publish_filament_columns(config, *slot))
            if (column < variants->size() && std::string_view(variants->get_at(column)) == variant)
                return column;
        return std::nullopt;
    }
    for (size_t local = 0; local < variants->size(); ++local)
        if (std::string_view(variants->get_at(local)) == variant)
            return local;
    return std::nullopt;
}

PublishResolvedColumn publish_resolved_column(const DynamicPrintConfig &uncompacted, size_t slot)
{
    PublishResolvedColumn out;
    const std::vector<size_t> columns = publish_filament_columns(uncompacted, slot);
    if (columns.empty())
        return out;

    const ConfigOptionStrings *variants = filament_variant_list(uncompacted);
    out.file_column = columns.front();
    if (variants != nullptr && columns.front() < variants->size())
        out.used_variant = variants->get_at(columns.front());

    const ConfigOptionInts          *filament_map      = uncompacted.option<ConfigOptionInts>("filament_map");
    const ConfigOptionInts          *filament_volumes  = uncompacted.option<ConfigOptionInts>("filament_volume_map");
    const ConfigOptionEnumsGeneric  *extruder_types    = uncompacted.option<ConfigOptionEnumsGeneric>("extruder_type");
    const ConfigOptionEnumsGeneric  *nozzle_volumes    = uncompacted.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (extruder_types == nullptr || nozzle_volumes == nullptr)
        return out;

    // full_fff_config pads a short (or missing) map with 1 / Standard, so a one-element map on a
    // multi-slot project still resolves every slot; mirror that instead of bailing out.
    const int extruder = (filament_map != nullptr && !filament_map->empty() && slot < filament_map->size()) ?
                             filament_map->get_at(slot) :
                             1;
    if (extruder < 1 || size_t(extruder) > extruder_types->size() || size_t(extruder) > nozzle_volumes->size())
        return out;

    NozzleVolumeType filament_nozzle = nvtStandard;
    // full_fff_config uses the volume map only when it is sized to the filament count; a partially
    // sized map is stale state and means "Standard" for every slot.
    const auto *colours = uncompacted.option<ConfigOptionStrings>("filament_colour");
    const auto *diameters = uncompacted.option<ConfigOptionFloats>("filament_diameter");
    const size_t filament_count = colours != nullptr ? colours->size() : (diameters != nullptr ? diameters->size() : 0);
    if (filament_volumes != nullptr && filament_count > 0 && filament_volumes->size() == filament_count &&
        slot < filament_count)
        filament_nozzle = NozzleVolumeType(filament_volumes->get_at(slot));
    // nvtHybrid is not a preset variant: get_index_for_extruder matches it as Standard.
    if (filament_nozzle == nvtHybrid)
        filament_nozzle = nvtStandard;

    // A hybrid extruder has no preset column of its own: the slot's own volume type selects it,
    // exactly as full_fff_config resolves it.
    const NozzleVolumeType printer_nozzle = NozzleVolumeType(nozzle_volumes->get_at(extruder - 1));
    const NozzleVolumeType nozzle         = printer_nozzle == nvtHybrid ? filament_nozzle : printer_nozzle;
    out.required_variant = get_extruder_variant_string(ExtruderType(extruder_types->get_at(extruder - 1)), nozzle);
    if (out.required_variant.empty())
        return out;

    if (const std::optional<size_t> exact = publish_match_variant(uncompacted, slot, out.required_variant); exact.has_value()) {
        out.file_column  = *exact;
        out.exact        = true;
        out.used_variant = out.required_variant;
    }
    return out;
}

void publish_normalize_material_entries(std::vector<PublishedMaterialEntry> &entries,
                                        const DynamicPrintConfig &uncompacted,
                                        std::vector<std::string> *notices)
{
    const std::set<std::string> &mixed_keys = publish_mixed_keys();
    for (PublishedMaterialEntry &entry : entries) {
        if (entry.slot < 0 || entry.full)
            continue;
        // A mixed definition is a project-level per-slot array: no variant-bearing keys.
        bool is_mixed = false;
        for (const std::string &key : entry.keys)
            if (mixed_keys.count(publish_base_key(key)) != 0) {
                is_mixed = true;
                break;
            }
        if (is_mixed)
            continue;

        const PublishResolvedColumn resolved = publish_resolved_column(uncompacted, size_t(entry.slot));
        const std::vector<size_t>  columns  = publish_filament_columns(uncompacted, size_t(entry.slot));
        // No layout for this slot (legacy identity config): leave the keys untouched and let the
        // filter's slot-indexed fallback mask them.
        if (columns.empty())
            continue;
        if (!resolved.exact && notices != nullptr) {
            // One notice per partial slot that has variant-bearing keys to write.
            const bool has_variant_key = std::any_of(entry.keys.begin(), entry.keys.end(), [](const std::string &key) {
                return filament_options_with_variant.count(publish_base_key(key)) != 0;
            });
            if (has_variant_key && resolved.file_column.has_value())
                notices->emplace_back((boost::format(L("Slot %1%: the printer needs the \"%2%\" variant, which the filament does not "
                                                       "define. Publishing the values printed with \"%3%\".")) %
                                       (entry.slot + 1) % resolved.required_variant % resolved.used_variant)
                                          .str());
        }

        auto in_slot = [&columns](int column) {
            return column >= 0 && std::find(columns.begin(), columns.end(), size_t(column)) != columns.end();
        };
        std::vector<std::string> keys;
        keys.reserve(entry.keys.size());
        std::set<std::string> seen;
        for (const std::string &key : entry.keys) {
            const std::string base = publish_base_key(key);
            std::string normalized  = key;
            if (filament_options_with_variant.count(base) != 0) {
                if (const int column = publish_key_index(key); column >= 0) {
                    // A caller-supplied column must address this slot; never ship another slot's value.
                    if (!in_slot(column)) {
                        BOOST_LOG_TRIVIAL(warning) << "publish: dropping \"" << key << "\" from slot " << (entry.slot + 1)
                                                   << " (the column belongs to another slot or is out of range)";
                        continue;
                    }
                } else {
                    normalized = base + "#" + std::to_string(*resolved.file_column);
                }
            }
            if (seen.insert(normalized).second)
                keys.push_back(std::move(normalized));
        }
        entry.keys = std::move(keys);
    }
}

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

void make_publish_universal(DynamicPrintConfig &config)
{
    // Lists: empty => compatible with every printer / every print preset. Conditions:
    // empty so a leftover expression left behind by the baseline clone can never
    // re-narrow the match (see is_compatible_with_printer, Preset.cpp:840). All four
    // keys exist on filament presets; nil-guard for hand-crafted future schemas.
    if (auto *opt = config.opt<ConfigOptionStrings>("compatible_printers", false))
        opt->values.clear();
    if (auto *opt = config.opt<ConfigOptionStrings>("compatible_prints", false))
        opt->values.clear();
    if (auto *opt = config.opt<ConfigOptionString>("compatible_printers_condition", false))
        opt->value.clear();
    if (auto *opt = config.opt<ConfigOptionString>("compatible_prints_condition", false))
        opt->value.clear();
}

std::string publish_material_base_name(const std::string &preset_name)
{
    if (preset_name.empty())
        return preset_name;
    const size_t at = preset_name.find('@');
    std::string base = (at == std::string::npos) ? preset_name : preset_name.substr(0, at);
    boost::trim_right(base);
    return base;
}

const std::set<std::string>& publish_structural_keys()
{
    // Non-publishable keys: the *_settings_id keys are also in PresetCollection::skipped_in_dirty
    // (Preset.cpp) / stripped from configs (profile_print_params_same); publishing them would
    // rewrite the user's preset inheritance/structure.
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

const std::set<std::string>& publish_mixed_keys()
{
    // Must match PresetBundle's s_project_options mixed-color group (PresetBundle.cpp): these
    // are project-level parallel per-slot arrays, not filament-preset options, so the import
    // material pass applies them into project_config instead of a filament preset config.
    static const std::set<std::string> mixed_keys = {
        "filament_is_mixed",
        "filament_mixed_components",
        "filament_mixed_sublayer_ratios",
        "filament_mixed_gradient",
        "filament_mixed_gradient_range",
        "filament_mixed_gradient_curve",
        "filament_mixed_gradient_per_part"
    };
    return mixed_keys;
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
    // Union of the two optgroups; "Retraction when switching material" keys are excluded
    // (toolchange retraction is device/profile territory, not a publishable behavior tweak).
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
    std::set<std::string> keys;

    // Union the dirty keys of each collection's edited preset (filaments may span multiple
    // slots); feeds only the Publish dialog's pre-check.
    for (const std::string& key : bundle.prints.current_dirty_options(true))
        keys.insert(key);
    for (const std::string& key : bundle.printers.current_dirty_options(true))
        keys.insert(key);
    for (const std::string& key : bundle.filaments.current_dirty_options(true))
        keys.insert(key);

    return std::vector<std::string>(keys.begin(), keys.end());
}

DynamicPrintConfig filter_published_config(
    const DynamicPrintConfig &full_config,
    const std::vector<std::string> &published_keys,
    const std::vector<PublishedMaterialEntry> &material_keys)
{
    DynamicPrintConfig filtered;

    std::set<std::string> base_keys_to_include;
    // Never masked (whole-vector serialization): identity, plate geometry, process keys and
    // printer keys without a "#N" variant.
    std::set<std::string>         mask_exempt_keys;
    // Material entries: base key -> author slots/columns whose values must survive; other
    // elements are masked to their defaults so a publish (partial or full) does not leak
    // unrelated slot data.
    std::map<std::string, std::set<int>> slot_mask_map;
    // Base keys whose mask comes from a material entry: an out-of-range index drops the key.
    // Printer and process keys keep their existing behaviour (whole vector or variant mask).
    std::set<std::string> material_masked_keys;
    // Material keys masked by file column (not by slot): the flattened vector must be as long as
    // the file's variant layout before any column can be trusted.
    std::set<std::string> column_masked_keys;

    // 1. Mandatory material identity & slot count keys for 3MF validation/normalization
    // (filament_ids: exported for validation, denylisted on apply - see publish_structural_keys).
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

    // 3. Process and printer published keys. Per-extruder keys carry a "#N" variant (e.g.
    // retraction_length#2, outer_wall_speed#1): mask the base to the author's extruder index so a
    // partial publish does not serialize every extruder's value (same slot-masking as the material side).
    const std::set<std::string> &printer_keys = publishable_printer_keys();
    for (const std::string &key : published_keys) {
        const std::string base_key = publish_base_key(key);
        if (base_key.empty())
            continue;
        base_keys_to_include.insert(base_key);
        if (printer_keys.count(base_key) != 0 || print_options_with_variant.count(base_key) != 0) {
            const int variant_idx = publish_key_index(key);
            if (variant_idx >= 0)
                slot_mask_map[base_key].insert(variant_idx);
            else
                mask_exempt_keys.insert(base_key); // bare key or malformed variant: whole vector
        } else {
            mask_exempt_keys.insert(base_key); // other process keys: whole vector
        }
    }

    // 4. Material-specific published keys. A variant-bearing key (filament_options_with_variant)
    // addresses a file column ("key#col", written by publish_normalize_material_entries); every
    // other filament vector key is addressed by slot. Full-publish entries mask to every column of
    // the author's slot. Slot-less entries (hand-crafted files) stay unmasked (whole vector).
    //
    // A config whose filament_self_index is still the unexpanded identity layout (one variant per
    // slot, e.g. a hand-built config or an old fixture) has no columns to resolve; there the slot
    // index is the safe mask, as it was before variant columns existed. A real column layout with
    // no column for the slot cannot be masked safely, so the key is dropped.
    const auto *self_index = full_config.option<ConfigOptionInts>("filament_self_index");
    const bool  identity_layout = self_index == nullptr || self_index->empty() ||
                                  std::all_of(self_index->values.begin(), self_index->values.end(),
                                              [index = 0](int id) mutable { return id == ++index; });
    auto add_material_key = [&](const PublishedMaterialEntry &entry, const std::string &key) {
        const std::string base_key = publish_base_key(key);
        if (base_key.empty())
            return;
        if (entry.slot < 0) {
            base_keys_to_include.insert(base_key); // hand-crafted, slot-less: whole vector
            return;
        }
        if (filament_options_with_variant.count(base_key) == 0) {
            base_keys_to_include.insert(base_key);
            slot_mask_map[base_key].insert(entry.slot);
            material_masked_keys.insert(base_key);
            return;
        }

        const std::vector<size_t> columns  = publish_filament_columns(full_config, size_t(entry.slot));
        const int                 supplied = publish_key_index(key);
        if (supplied >= 0) {
            // A caller-supplied column must address this slot; never ship another slot's value.
            const bool valid = columns.empty() ? supplied == entry.slot :
                                                 std::find(columns.begin(), columns.end(), size_t(supplied)) != columns.end();
            if (!valid) {
                BOOST_LOG_TRIVIAL(warning) << "publish: dropping material key \"" << key << "\" of slot " << (entry.slot + 1)
                                           << " (the column belongs to another slot or is out of range)";
                return;
            }
        }
        if (columns.empty() && !identity_layout) {
            BOOST_LOG_TRIVIAL(warning) << "publish: dropping material key \"" << key << "\" of slot " << (entry.slot + 1)
                                       << " (the file layout has no variant column for this slot)";
            return;
        }
        base_keys_to_include.insert(base_key);

        if (entry.full) {
            if (columns.empty()) {
                // Identity layout: one column per slot, so the slot index is the column.
                slot_mask_map[base_key].insert(entry.slot);
                material_masked_keys.insert(base_key);
            } else {
                for (const size_t column : columns)
                    slot_mask_map[base_key].insert(int(column));
                material_masked_keys.insert(base_key);
                column_masked_keys.insert(base_key);
            }
            return;
        }
        if (supplied >= 0) {
            if (columns.empty()) {
                slot_mask_map[base_key].insert(entry.slot);
                material_masked_keys.insert(base_key);
            } else {
                slot_mask_map[base_key].insert(supplied);
                material_masked_keys.insert(base_key);
                column_masked_keys.insert(base_key);
            }
            return;
        }
        // Plain variant-bearing key (a selection from before variant-aware publishing): keep the
        // column the slot prints with, so the published value is the one the author printed.
        const PublishResolvedColumn resolved = publish_resolved_column(full_config, size_t(entry.slot));
        if (resolved.file_column.has_value() && !columns.empty()) {
            slot_mask_map[base_key].insert(int(*resolved.file_column));
            material_masked_keys.insert(base_key);
            column_masked_keys.insert(base_key);
        } else {
            // Identity layout: keep the old slot-indexed masking.
            slot_mask_map[base_key].insert(entry.slot);
            material_masked_keys.insert(base_key);
        }
    };
    for (const PublishedMaterialEntry &entry : material_keys) {
        for (const std::string &key : entry.keys)
            add_material_key(entry, key);
        for (const std::string &key : entry.full_keys)
            add_material_key(entry, key);
    }

    // Masking restores every non-published slot of a vector option with the option default, so
    // a partial publish does not leak unrelated slot data. can_mask_slots reports whether a key
    // is maskable at all (vector option plus a registered default of the same type); an
    // unmaskable key is dropped from the payload entirely instead of shipping the author's
    // whole vector.
    auto can_mask_slots = [](const ConfigOption &opt, const ConfigOptionDef *def) -> bool {
        if (def == nullptr || !def->default_value || def->default_value->type() != opt.type())
            return false;
        const auto *vec         = dynamic_cast<const ConfigOptionVectorBase *>(&opt);
        const auto *default_vec = dynamic_cast<const ConfigOptionVectorBase *>(def->default_value.get());
        return vec != nullptr && vec->size() > 0 && default_vec != nullptr && !default_vec->empty();
    };
    auto mask_slots = [](ConfigOption &opt, const ConfigOptionDef *def, const std::set<int> &keep_slots) {
        auto *vec = dynamic_cast<ConfigOptionVectorBase*>(&opt);
        for (size_t idx = 0; idx < vec->size(); ++idx)
            if (keep_slots.count(static_cast<int>(idx)) == 0)
                vec->set_at(def->default_value.get(), idx, 0);
    };

    // Copy the selected options from full_config into the filtered config.
    for (const std::string &key : base_keys_to_include) {
        const ConfigOption *opt = full_config.option(key);
        if (opt == nullptr)
            continue;
        const auto  mask_it       = slot_mask_map.find(key);
        const bool  needs_masking = mask_exempt_keys.count(key) == 0 && mask_it != slot_mask_map.end() && !mask_it->second.empty();
        if (needs_masking && !can_mask_slots(*opt, print_config_def.get(key))) {
            BOOST_LOG_TRIVIAL(warning) << "publish: dropping unmaskable key \"" << key
                                       << "\" from the published payload (no usable option default)";
            continue;
        }
        if (needs_masking && column_masked_keys.count(key) != 0) {
            // The flattened vector must be as long as the file's variant layout. A preset whose
            // variant vector is shorter shifts every later column, so a column that looks in range
            // can hold another slot's value: drop the key instead.
            const auto  *self_index = full_config.option<ConfigOptionInts>("filament_self_index");
            const auto  *variants   = full_config.option<ConfigOptionStrings>("filament_extruder_variant");
            const size_t layout_sz  = self_index != nullptr ? self_index->size() :
                                      (variants != nullptr ? variants->size() : size_t(0));
            const auto  *vec        = dynamic_cast<const ConfigOptionVectorBase *>(opt);
            if (layout_sz > 0 && vec->size() != layout_sz) {
                BOOST_LOG_TRIVIAL(warning) << "publish: dropping key \"" << key
                                           << "\" from the published payload (its variant vector does not match the file layout)";
                continue;
            }
        }
        if (needs_masking && material_masked_keys.count(key) != 0) {
            // A selected column/slot the author's vector does not have carries no value: drop the
            // key rather than shipping the option default as if it were the author's setting.
            const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(opt);
            const bool  in_range = std::all_of(mask_it->second.begin(), mask_it->second.end(),
                                               [vec](int index) { return index >= 0 && size_t(index) < vec->size(); });
            if (!in_range) {
                BOOST_LOG_TRIVIAL(warning) << "publish: dropping key \"" << key
                                           << "\" from the published payload (selected variant column is out of range)";
                continue;
            }
        }
        ConfigOption *cloned = opt->clone();
        if (needs_masking)
            mask_slots(*cloned, print_config_def.get(key), mask_it->second);
        filtered.set_key_value(key, cloned);
    }

    return filtered;
}

int resolve_published_extruder_slot(const PublishedExtruderLayout &author, size_t author_index,
                                    const DynamicPrintConfig &receiver_printer, const DynamicPrintConfig &receiver_project)
{
    return resolve_published_extruder_slot(author, author_index, receiver_printer, receiver_project, receiver_printer,
                                           "printer_extruder_id", "printer_extruder_variant");
}

int resolve_published_extruder_slot(const PublishedExtruderLayout &author, size_t author_index,
                                    const DynamicPrintConfig &receiver_printer, const DynamicPrintConfig &receiver_project,
                                    const DynamicPrintConfig &receiver_target, const std::string &id_key, const std::string &variant_key)
{
    if (author_index >= author.extruder_ids.size() || author_index >= author.variants.size())
        return -1;
    const int author_extruder = author.extruder_ids[author_index];
    const auto *nozzles        = receiver_printer.option<ConfigOptionFloats>("nozzle_diameter");
    const auto *extruder_types = receiver_printer.option<ConfigOptionEnumsGeneric>("extruder_type");
    const auto *volumes        = receiver_project.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (nozzles == nullptr || extruder_types == nullptr || volumes == nullptr || author_extruder < 1 ||
        size_t(author_extruder) > nozzles->size())
        return -1;
    const size_t e = size_t(author_extruder - 1);
    if (e >= extruder_types->size() || e >= volumes->size())
        return -1;
    const ExtruderType     extruder_type = ExtruderType(extruder_types->values[e]);
    const NozzleVolumeType nozzle_type   = NozzleVolumeType(volumes->values[e]);
    if (nozzle_type == nvtHybrid || author.variants[author_index] != get_extruder_variant_string(extruder_type, nozzle_type))
        return -1;
    return receiver_target.get_index_for_extruder(author_extruder, id_key, extruder_type, nozzle_type, variant_key);
}

} // namespace Slic3r
