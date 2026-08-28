#include "MixedFilamentPaintBake.hpp"

#include "MixedFilament.hpp"
#include "MixedFilamentMatch.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace Slic3r {

namespace {

void fill_identity(EnforcerBlockerStateMap &slot_map)
{
    for (size_t i = 0; i < slot_map.size(); ++i)
        slot_map[i] = EnforcerBlockerType(i);
}

} // namespace

SpectrumPaintBakePlan plan_spectrum_paint_bake(
    const std::vector<std::string> &source_hexes,
    const std::vector<ColorRGB>    &physicals,
    size_t                          mix_base)
{
    SpectrumPaintBakePlan plan;
    fill_identity(plan.slot_map);

    if (source_hexes.empty()) {
        plan.error = "Source palette is empty. Adopt first.";
        return plan;
    }
    if (source_hexes.size() > SPECTRUM_PAINT_ID_PERSIST_CAP) {
        plan.error = "Source palette has more than 15 colours; mapping refused.";
        return plan;
    }

    const size_t num_match_slots = physicals.size() < size_t(4) ? physicals.size() : size_t(4);
    std::vector<ColorRGB> slots;
    slots.reserve(num_match_slots);
    for (size_t i = 0; i < num_match_slots; ++i)
        slots.push_back(physicals[i]);

    std::vector<std::string> recipe_rows;
    std::unordered_map<std::string, unsigned> recipe_to_mix_id;
    recipe_to_mix_id.reserve(source_hexes.size());
    // Delay Mix dest writes into slot_map until persist-cap is known-ok.
    std::vector<unsigned> pending_mix_dest(source_hexes.size() + 1, 0);

    for (size_t src = 1; src <= source_hexes.size(); ++src) {
        const std::string normalized = normalize_mix_match_hex(source_hexes[src - 1]);
        ColorRGB target;
        const bool decoded = !normalized.empty() && decode_color(normalized, target);
        if (!decoded) {
            BOOST_LOG_TRIVIAL(warning) << "spectrum paint bake: invalid source hex at index " << src
                                       << " '" << source_hexes[src - 1] << "'; mapping to physical 1";
            plan.slot_map[src] = EnforcerBlockerType::Extruder1;
            ++plan.physical_mapped_count;
            continue;
        }

        const MixMatchResult match = match_printable_mix(target, slots, nullptr);
        if (!match.valid || (match.kind == MixMatchResult::Kind::Mix && match.recipe_row.empty())) {
            BOOST_LOG_TRIVIAL(warning) << "spectrum paint bake: match failed for source index " << src
                                       << "; mapping to physical 1";
            plan.slot_map[src] = EnforcerBlockerType::Extruder1;
            ++plan.physical_mapped_count;
            continue;
        }

        if (match.kind == MixMatchResult::Kind::Physical) {
            unsigned pid = match.physical_id;
            if (pid < 1)
                pid = 1;
            if (num_match_slots > 0 && pid > num_match_slots)
                pid = unsigned(num_match_slots);
            plan.slot_map[src] = EnforcerBlockerType(pid);
            ++plan.physical_mapped_count;
            continue;
        }

        const auto found = recipe_to_mix_id.find(match.recipe_row);
        unsigned dest_id = 0;
        if (found != recipe_to_mix_id.end()) {
            dest_id = found->second;
        } else {
            dest_id = unsigned(mix_base + recipe_rows.size() + 1);
            recipe_to_mix_id.emplace(match.recipe_row, dest_id);
            recipe_rows.push_back(match.recipe_row);
        }
        pending_mix_dest[src] = dest_id;
    }

    plan.mix_count = recipe_rows.size();
    if (mix_base + plan.mix_count > SPECTRUM_PAINT_ID_PERSIST_CAP) {
        plan.error = "Physical filaments plus unique mixes exceed the persist cap of 15.";
        plan.mix_count             = 0;
        plan.physical_mapped_count = 0;
        plan.mixed_filament_definitions.clear();
        fill_identity(plan.slot_map);
        return plan;
    }

    for (size_t src = 1; src <= source_hexes.size(); ++src) {
        if (pending_mix_dest[src] != 0)
            plan.slot_map[src] = EnforcerBlockerType(pending_mix_dest[src]);
    }

    for (size_t j = 1; j <= size_t(EnforcerBlockerType::ExtruderMax); ++j) {
        if (j <= source_hexes.size())
            continue;
        plan.slot_map[j] = EnforcerBlockerType::Extruder1;
    }

    if (!recipe_rows.empty()) {
        std::string serialized;
        for (size_t i = 0; i < recipe_rows.size(); ++i) {
            if (i)
                serialized += ';';
            serialized += recipe_rows[i];
        }
        plan.mixed_filament_definitions = std::move(serialized);
    }

    plan.valid = true;
    return plan;
}

bool apply_spectrum_paint_bake(ModelVolume &vol, const EnforcerBlockerStateMap &slot_map)
{
    TriangleSelector selector(vol.mesh());
    selector.deserialize(vol.mmu_segmentation_facets.get_data(), true, EnforcerBlockerType::ExtruderMax);
    selector.remap_triangle_state(slot_map);
    vol.mmu_segmentation_facets.set(selector);
    return true;
}

bool apply_spectrum_map_keys(DynamicPrintConfig        &project_config,
                             const SpectrumMapUndoKeys &keys,
                             DynamicPrintConfig        *print_config)
{
    bool changed = false;

    const ConfigOptionBool *cur_mapped = project_config.option<ConfigOptionBool>("spectrum_paint_mapped");
    if (cur_mapped == nullptr || cur_mapped->value != keys.mapped) {
        project_config.set_key_value("spectrum_paint_mapped", new ConfigOptionBool(keys.mapped));
        changed = true;
    }

    const ConfigOptionString *cur_mix = project_config.option<ConfigOptionString>("mixed_filament_definitions");
    if (cur_mix == nullptr || cur_mix->value != keys.mixed_filament_definitions) {
        project_config.set_key_value("mixed_filament_definitions",
                                     new ConfigOptionString(keys.mixed_filament_definitions));
        changed = true;
    }

    if (print_config != nullptr) {
        const ConfigOptionString *print_mix =
            print_config->option<ConfigOptionString>("mixed_filament_definitions");
        if (print_mix == nullptr || print_mix->value != keys.mixed_filament_definitions) {
            print_config->set_key_value("mixed_filament_definitions",
                                        new ConfigOptionString(keys.mixed_filament_definitions));
            changed = true;
        }
    }

    return changed;
}

} // namespace Slic3r
