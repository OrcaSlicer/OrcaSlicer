#include "WipeTowerEstimate.hpp"

#include "WipeTower.hpp"
#include "WipeTower2.hpp"
#include "../Config.hpp"
#include "../PrintConfig.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>

namespace Slic3r {

WipeTowerFootprint estimate_wipe_tower_footprint(const ConfigBase &config, size_t filaments_cnt, double layer_height, double max_object_height, bool any_raft)
{
    WipeTowerFootprint footprint;
    footprint.height = max_object_height;
    if (filaments_cnt == 0 || layer_height < EPSILON)
        return footprint;

    // Every caller today declares all these keys, but the signature accepts any ConfigBase:
    // fall back to the key's declared default, never to a hand-copied constant.
    auto option_of = [&config](const char *key) -> const ConfigOption * {
        if (const ConfigOption *opt = config.option(key); opt != nullptr)
            return opt;
        if (const ConfigDef *def = config.def(); def != nullptr)
            if (const ConfigOptionDef *opt_def = def->get(key); opt_def != nullptr)
                return opt_def->default_value.get();
        return nullptr;
    };
    auto opt_float = [&option_of](const char *key) {
        const ConfigOption *opt = option_of(key);
        return opt != nullptr ? opt->getFloat() : 0.;
    };
    auto opt_bool = [&option_of](const char *key) {
        const ConfigOption *opt = option_of(key);
        return opt != nullptr && opt->getBool();
    };
    auto max_of = [&option_of](const char *key, double fallback) {
        const auto *opt = dynamic_cast<const ConfigOptionFloats *>(option_of(key));
        return (opt != nullptr && !opt->values.empty()) ? *std::max_element(opt->values.begin(), opt->values.end()) : fallback;
    };

    const double width            = opt_float("prime_tower_width");
    const double prime_volume     = opt_float("prime_volume");
    const double extra_spacing    = opt_float("prime_tower_infill_gap") / 100.;
    double       rib_width        = opt_float("wipe_tower_rib_width");
    const double extra_rib_length = opt_float("wipe_tower_extra_rib_length");
    const auto  *nozzle_opt       = dynamic_cast<const ConfigOptionFloats *>(option_of("nozzle_diameter"));
    const bool   dual_nozzle      = nozzle_opt != nullptr && nozzle_opt->values.size() == 2;
    const auto  *wall_type_opt    = dynamic_cast<const ConfigOptionEnum<WipeTowerWallType> *>(option_of("wipe_tower_wall_type"));
    const bool   rib_wall         = wall_type_opt != nullptr && wall_type_opt->value == WipeTowerWallType::wtwRib;
    const auto  *timelapse_opt    = dynamic_cast<const ConfigOptionEnum<TimelapseType> *>(option_of("timelapse_type"));
    const bool   smooth_timelapse = timelapse_opt != nullptr && timelapse_opt->value == TimelapseType::tlSmooth;
    // Reasons a tower is printed with no tool change to purge for.
    const bool   need_wipe_tower  = smooth_timelapse || opt_bool("enable_wrapping_detection") || any_raft;

    // No tool change, nothing to purge; smooth timelapse still primes once.
    size_t purge_count = 0;
    if (filaments_cnt > 1)
        purge_count = dual_nozzle ? filaments_cnt : filaments_cnt - 1;
    else if (smooth_timelapse)
        purge_count = 1;

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

    // Both wall types decide this together: over-reserving only wastes bed area, but
    // reporting no tower for one that is built collapses the validation hull to a point.
    if (volume < EPSILON && !need_wipe_tower)
        return footprint;

    const double min_depth = WipeTower::get_limit_depth_by_height(float(max_object_height));
    if (rib_wall) {
        // A rib wall squares the tower; the ribs run the diagonal and bulge past the body.
        const double volume_depth = std::sqrt(volume / layer_height * extra_spacing);
        double       depth        = std::max(min_depth, volume_depth);
        rib_width                 = std::min(rib_width, depth / 2.);
        depth                     = rib_width / std::sqrt(2.) + std::max(depth + extra_rib_length, volume_depth);
        footprint.width = footprint.depth = depth;
    } else {
        double depth = volume / (layer_height * width);
        // The flush volumes already hold the spacing between wipes.
        if (!semm_flush)
            depth *= extra_spacing;
        footprint.width = width;
        footprint.depth = std::max(min_depth, depth);
    }

    footprint.brim_width = opt_float("prime_tower_brim_width");
    if (footprint.brim_width < 0)
        footprint.brim_width = WipeTower::get_auto_brim_by_height(float(max_object_height));
    return footprint;
}

} // namespace Slic3r
