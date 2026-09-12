#include "WipeTowerEstimate.hpp"

#include "WipeTower.hpp"
#include "WipeTower2.hpp"
#include "../Config.hpp"
#include "../PrintConfig.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r {

// Every caller today declares all these keys, but the signature accepts any ConfigBase: fall
// back to the key's declared default, never to a hand-copied constant.
static const ConfigOption *option_of(const ConfigBase &config, const char *key)
{
    if (const ConfigOption *opt = config.option(key); opt != nullptr)
        return opt;
    if (const ConfigDef *def = config.def(); def != nullptr)
        if (const ConfigOptionDef *opt_def = def->get(key); opt_def != nullptr)
            return opt_def->default_value.get();
    return nullptr;
}

WipeTowerType resolve_wipe_tower_type(const ConfigBase &config)
{
    // printer_model is what the CLI keys its Bambu Lab detection on; the GUI's vendor flag
    // agrees for every shipped profile.
    if (const auto *model = dynamic_cast<const ConfigOptionString *>(config.option("printer_model"));
        model != nullptr && model->value.compare(0, 9, "Bambu Lab") == 0)
        return WipeTowerType::Type1;
    // By value, not by concrete type: a static PrintConfig holds ConfigOptionEnum<T>, a
    // DynamicConfig built from presets holds ConfigOptionEnumGeneric, and both answer getInt().
    const ConfigOption *type = option_of(config, "wipe_tower_type");
    return type != nullptr ? WipeTowerType(type->getInt()) : WipeTowerType::Type2;
}

Polygon estimate_wipe_tower_first_layer_outline(const ConfigBase &config, WipeTowerType tower_type, double width, double depth, double height)
{
    // Type1 ignores the cone option. The wall type is read by value: a preset-shaped config
    // holds it as ConfigOptionEnumGeneric, which a cast to ConfigOptionEnum<T> cannot see.
    const ConfigOption *wall_type  = option_of(config, "wipe_tower_wall_type");
    const ConfigOption *cone_angle = option_of(config, "wipe_tower_cone_angle");
    const bool          cone       = tower_type == WipeTowerType::Type2 && wall_type != nullptr &&
                         wall_type->getInt() == int(WipeTowerWallType::wtwCone) && cone_angle != nullptr;
    return WipeTower2::cone_base_polygon(width, depth, height, cone ? cone_angle->getFloat() : 0.);
}

WipeTowerFootprint estimate_wipe_tower_footprint(const ConfigBase &config, WipeTowerType tower_type, const std::vector<unsigned int> &filament_ids, double layer_height, double max_object_height)
{
    WipeTowerFootprint footprint;
    footprint.height = max_object_height;
    const size_t filaments_cnt = filament_ids.size();
    if (filaments_cnt == 0 || layer_height < EPSILON)
        return footprint;

    auto opt_float = [&config](const char *key) {
        const ConfigOption *opt = option_of(config, key);
        return opt != nullptr ? opt->getFloat() : 0.;
    };
    auto opt_bool = [&config](const char *key) {
        const ConfigOption *opt = option_of(config, key);
        return opt != nullptr && opt->getBool();
    };
    auto opt_enum = [&config](const char *key, int fallback) {
        const ConfigOption *opt = option_of(config, key);
        return opt != nullptr ? opt->getInt() : fallback;
    };
    auto floats_of = [&config](const char *key) { return dynamic_cast<const ConfigOptionFloats *>(option_of(config, key)); };
    auto max_of = [&floats_of](const char *key, double fallback) {
        const auto *opt = floats_of(key);
        return (opt != nullptr && !opt->values.empty()) ? *std::max_element(opt->values.begin(), opt->values.end()) : fallback;
    };
    auto float_at = [&floats_of](const char *key, unsigned int id, double fallback) {
        const auto *opt = floats_of(key);
        return (opt != nullptr && !opt->values.empty()) ? opt->get_at(id) : fallback;
    };
    auto int_at = [&config](const char *key, unsigned int id, int fallback) {
        const auto *opt = dynamic_cast<const ConfigOptionInts *>(option_of(config, key));
        return (opt != nullptr && !opt->values.empty()) ? opt->get_at(id) : fallback;
    };

    // Both planners size every layer, so the tower has to fit its thinnest one: the first layer
    // when it is printed thinner than the rest.
    const double first_layer_height = opt_float("initial_layer_print_height");
    if (first_layer_height > EPSILON)
        layer_height = std::min(layer_height, first_layer_height);

    const bool   type1            = tower_type == WipeTowerType::Type1;
    const double width            = opt_float("prime_tower_width");
    const double prime_volume     = opt_float("prime_volume");
    // Type1 spaces its purge lines by prime_tower_infill_gap, Type2 by wipe_tower_extra_spacing.
    // Type2's extra flow cancels out of the depth: the line length is divided by it and the row
    // pitch multiplied by it (WipeTower2::get_wipe_depth).
    const double extra_spacing    = opt_float(type1 ? "prime_tower_infill_gap" : "wipe_tower_extra_spacing") / 100.;
    const double rib_width        = opt_float("wipe_tower_rib_width");
    const double extra_rib_length = opt_float("wipe_tower_extra_rib_length");
    const auto  *nozzle_opt       = floats_of("nozzle_diameter");
    const double nozzle_diameter  = (nozzle_opt != nullptr && !nozzle_opt->values.empty()) ? nozzle_opt->values.front() : 0.4;
    const bool   dual_nozzle      = nozzle_opt != nullptr && nozzle_opt->values.size() == 2;
    const bool   rib_wall         = opt_enum("wipe_tower_wall_type", int(WipeTowerWallType::wtwRectangle)) == int(WipeTowerWallType::wtwRib);
    const bool   smooth_timelapse = opt_enum("timelapse_type", int(TimelapseType::tlTraditional)) == int(TimelapseType::tlSmooth);
    const bool   wrapping         = opt_bool("enable_wrapping_detection");
    // Reasons a tower is printed with no tool change to purge for: the ones that stop
    // normalize_fdm_2 clearing enable_prime_tower. Its mixed-filament case is not modelled.
    const bool   need_wipe_tower  = smooth_timelapse || wrapping;

    // A tower printed for one of the reasons above has no tool change to purge for; both
    // planners give it the idle depth below and nothing more.
    const size_t purge_count = filaments_cnt > 1 ? (dual_nozzle ? filaments_cnt : filaments_cnt - 1) : 0;

    // Type2 purges one volume per tool change. Type1 plans per filament below; here the volume
    // only decides whether a tower exists.
    double volume = prime_volume * double(purge_count);
    if (dual_nozzle) {
        // Dual-nozzle printers also purge the filament change length on the tower.
        const double length   = max_of("filament_change_length", 0.);
        const double diameter = max_of("filament_diameter", 1.75);
        volume += length * PI * diameter * diameter / 4. * double(filaments_cnt / 2);
    }
    // Single-extruder multi-material purges the flush matrix instead of the prime volume.
    const bool semm_flush = opt_bool("purge_in_prime_tower") && opt_bool("single_extruder_multi_material");
    if (semm_flush)
        volume = WipeTower2::estimate_semm_flush_volume(config, filaments_cnt);

    // The Type1 planner wipes each filament's own prime volume after changing to it, in a block
    // per adhesiveness category. On a two-nozzle printer the leaving filament is also rammed at
    // every nozzle change; the tool order groups filaments by nozzle, so a layer crosses
    // (nozzles used - 1) times, charged here to the longest ramming.
    std::vector<WipeTower::PurgeEstimate> purges;
    if (type1 && filaments_cnt > 1) {
        const bool    saving_mode = opt_enum("prime_volume_mode", int(PrimeVolumeMode::pvmDefault)) == int(PrimeVolumeMode::pvmSaving);
        std::set<int> nozzles;
        size_t        longest_ramming = 0;
        for (size_t i = 0; i < filaments_cnt; ++i) {
            const unsigned int         id = filament_ids[i];
            WipeTower::PurgeEstimate purge;
            purge.prime_volume      = saving_mode ? 15.f : float(float_at("filament_prime_volume", id, prime_volume));
            purge.category          = int_at("filament_adhesiveness_category", id, 0);
            purge.filament_diameter = float(float_at("filament_diameter", id, 1.75));
            purges.push_back(purge);
            if (dual_nozzle) {
                nozzles.insert(int_at("filament_map", id, 1));
                if (float_at("filament_change_length", id, 0.) > float_at("filament_change_length", filament_ids[longest_ramming], 0.))
                    longest_ramming = i;
            }
        }
        if (nozzles.size() > 1)
            purges[longest_ramming].filament_change_length = float(float_at("filament_change_length", filament_ids[longest_ramming], 0.) * double(nozzles.size() - 1));
    }

    // Both wall types decide this together: over-reserving only wastes bed area, but reporting
    // no tower for one that is built collapses the validation hull to a point.
    // A tool change is a reason on its own (see the base commit); Type1 already reserves
    // per filament, Type2 has only the volume, which can resolve to zero.
    const bool has_purge = type1 ? !purges.empty() : volume > EPSILON;
    if (!has_purge && filaments_cnt < 2 && !need_wipe_tower)
        return footprint;

    const double min_depth      = WipeTower::get_limit_depth_by_height(float(max_object_height));
    const float  perimeter_width = float(nozzle_diameter) * 1.25f; // Width_To_Nozzle_Ratio
    // With nothing to purge, plan_tower_new sizes the tower for wrapping detection or the
    // stability minimum; WipeTower2 only knows the latter.
    const double idle_depth = (type1 && wrapping && !smooth_timelapse) ? WipeTower::get_wrapping_detection_depth() : min_depth;
    if (rib_wall) {
        // Both planners square the tower to the purge area and extend the ribs, not the body,
        // below the stability minimum.
        double side;
        if (!purges.empty())
            side = WipeTower::estimate_rib_tower_bbox_side(purges, float(width), float(layer_height), float(nozzle_diameter), float(extra_spacing), float(rib_width), float(extra_rib_length), float(max_object_height));
        else {
            const double square = has_purge ? std::sqrt(volume / layer_height * extra_spacing) : idle_depth;
            side = WipeTower::rib_footprint_side(float(square), float(square), float(rib_width), float(extra_rib_length), float(max_object_height));
        }
        footprint.width = footprint.depth = side;
    } else {
        double depth;
        if (type1) {
            // plan_tower_new stretches a short purge stack to the stability minimum behind its
            // leading perimeter width.
            depth = purges.empty() ? idle_depth : std::max(min_depth + perimeter_width, double(WipeTower::estimate_tower_blocks_depth(purges, float(width), float(layer_height), float(nozzle_diameter), float(extra_spacing))));
        } else {
            depth = volume / (layer_height * width);
            // The flush volumes already hold the spacing between wipes.
            if (!semm_flush)
                depth *= extra_spacing;
            depth = std::max(min_depth, depth);
        }
        footprint.width = width;
        footprint.depth = depth;
    }

    footprint.brim_width = opt_float("prime_tower_brim_width");
    if (footprint.brim_width < 0)
        footprint.brim_width = WipeTower::get_auto_brim_by_height(float(max_object_height));
    footprint.brim_width = WipeTower::estimate_brim_real_width(float(footprint.brim_width), float(nozzle_diameter), float(first_layer_height > EPSILON ? first_layer_height : layer_height), !type1);
    return footprint;
}

} // namespace Slic3r
