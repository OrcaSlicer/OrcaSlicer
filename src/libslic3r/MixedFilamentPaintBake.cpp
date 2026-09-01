#include "MixedFilamentPaintBake.hpp"

#include "MixedFilament.hpp"
#include "MixedFilamentMatch.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"
#include "prusa_fdm_mixer.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
        plan.error = "Source palette has more than " + std::to_string(SPECTRUM_PAINT_ID_PERSIST_CAP) +
                     " colours; mapping refused.";
        return plan;
    }

    const size_t num_match_slots = physicals.size() < size_t(4) ? physicals.size() : size_t(4);
    std::vector<ColorRGB> slots;
    slots.reserve(num_match_slots);
    for (size_t i = 0; i < num_match_slots; ++i)
        slots.push_back(physicals[i]);

    std::vector<std::string> recipe_rows;
    std::unordered_map<std::string, std::vector<size_t>> recipe_sources;
    recipe_sources.reserve(source_hexes.size());

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

        const MixMatchResult match = match_printable_mix(target, slots, nullptr, 4, 70);
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

        auto found = recipe_sources.find(match.recipe_row);
        if (found == recipe_sources.end()) {
            recipe_rows.push_back(match.recipe_row);
            recipe_sources.emplace(match.recipe_row, std::vector<size_t>{src});
        } else {
            found->second.push_back(src);
        }
    }

    // Snapmaker V2.3.6 Color Mixing Match Mapping approximates many source colours
    // onto four physicals. Fifteen unique sources cannot map 1:1 onto twelve dest
    // Mix IDs (paint persist 16 with four physicals). Collapse nearest Mix dests
    // by ColorMix ΔE00 until mix_base + unique Mix rows <= SPECTRUM_PAINT_ID_PERSIST_CAP.
    // Serialized rows stay ZR pair-prefix grammar — no Snapmaker keys.
    auto predicted_of_row = [&](const std::string &row) -> ColorRGB {
        MixedFilamentManager mgr;
        mgr.load_definitions(row);
        if (mgr.enabled_count() == 0)
            return ColorRGB::BLACK();
        return predicted_swatch_for_mix(mgr.mixed_filaments().front(), slots, nullptr);
    };
    auto mixer_de00 = [](const ColorRGB &a, const ColorRGB &b) -> float {
        const prusa_fdm_mixer::RGB ra{a.r_uchar(), a.g_uchar(), a.b_uchar()};
        const prusa_fdm_mixer::RGB rb{b.r_uchar(), b.g_uchar(), b.b_uchar()};
        return float(prusa_fdm_mixer::delta_e_2000(prusa_fdm_mixer::rgb_to_lab(ra),
                                                   prusa_fdm_mixer::rgb_to_lab(rb)));
    };

    while (mix_base + recipe_rows.size() > SPECTRUM_PAINT_ID_PERSIST_CAP && recipe_rows.size() >= 2) {
        size_t best_i = 0;
        size_t best_j = 1;
        float  best_d = 1e30f;
        for (size_t i = 0; i < recipe_rows.size(); ++i) {
            const ColorRGB pi = predicted_of_row(recipe_rows[i]);
            for (size_t j = i + 1; j < recipe_rows.size(); ++j) {
                const float d = mixer_de00(pi, predicted_of_row(recipe_rows[j]));
                if (d < best_d) {
                    best_d = d;
                    best_i = i;
                    best_j = j;
                }
            }
        }
        size_t keep = best_i;
        size_t drop = best_j;
        if (recipe_sources[recipe_rows[best_j]].size() > recipe_sources[recipe_rows[best_i]].size()) {
            keep = best_j;
            drop = best_i;
        }
        const std::string keep_row = recipe_rows[keep];
        const std::string drop_row = recipe_rows[drop];
        auto             &keep_srcs = recipe_sources[keep_row];
        const auto       &drop_srcs = recipe_sources[drop_row];
        keep_srcs.insert(keep_srcs.end(), drop_srcs.begin(), drop_srcs.end());
        recipe_sources.erase(drop_row);
        recipe_rows.erase(recipe_rows.begin() + int(drop));
    }

    plan.mix_count = recipe_rows.size();

    std::unordered_map<std::string, unsigned> recipe_to_mix_id;
    recipe_to_mix_id.reserve(recipe_rows.size());
    for (size_t i = 0; i < recipe_rows.size(); ++i)
        recipe_to_mix_id.emplace(recipe_rows[i], unsigned(mix_base + i + 1));

    for (const auto &kv : recipe_sources) {
        const auto dest_it = recipe_to_mix_id.find(kv.first);
        if (dest_it == recipe_to_mix_id.end())
            continue;
        const unsigned dest_id = dest_it->second;
        for (size_t src : kv.second)
            plan.slot_map[src] = EnforcerBlockerType(dest_id);
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

bool apply_spectrum_paint_mapped(DynamicPrintConfig &project_config, bool mapped)
{
    const ConfigOptionBool *cur_mapped = project_config.option<ConfigOptionBool>("spectrum_paint_mapped");
    if (cur_mapped != nullptr && cur_mapped->value == mapped)
        return false;
    project_config.set_key_value("spectrum_paint_mapped", new ConfigOptionBool(mapped));
    return true;
}

bool apply_spectrum_mix_dialog_keys(DynamicPrintConfig              &project_config,
                                    DynamicPrintConfig              &print_config,
                                    const SpectrumMixDialogUndoKeys &keys)
{
    bool changed = false;

    const ConfigOptionString *proj_mix =
        project_config.option<ConfigOptionString>("mixed_filament_definitions");
    if (proj_mix == nullptr || proj_mix->value != keys.mixed_filament_definitions) {
        project_config.set_key_value("mixed_filament_definitions",
                                     new ConfigOptionString(keys.mixed_filament_definitions));
        changed = true;
    }

    const ConfigOptionString *print_mix =
        print_config.option<ConfigOptionString>("mixed_filament_definitions");
    if (print_mix == nullptr || print_mix->value != keys.mixed_filament_definitions) {
        print_config.set_key_value("mixed_filament_definitions",
                                   new ConfigOptionString(keys.mixed_filament_definitions));
        changed = true;
    }

    auto apply_bool = [&](const char *key, bool value) {
        const ConfigOptionBool *cur = print_config.option<ConfigOptionBool>(key);
        if (cur == nullptr || cur->value != value) {
            print_config.set_key_value(key, new ConfigOptionBool(value));
            changed = true;
        }
    };
    apply_bool("enable_prime_tower", keys.enable_prime_tower);
    apply_bool("dithering_local_z_mode", keys.dithering_local_z_mode);
    apply_bool("dithering_local_z_whole_objects", keys.dithering_local_z_whole_objects);
    apply_bool("mixed_filament_gradient_mode", keys.mixed_filament_gradient_mode);

    return changed;
}

} // namespace Slic3r
