#pragma once

#include "../PrintConfig.hpp"
#include <cmath>
#include <string>

namespace Slic3r::Voronoi {

inline bool fused_density_modifier(const DynamicPrintConfig &config)
{
    const auto *blend = config.option<ConfigOptionFloat>("voronoi_modifier_blend");
    return blend && blend->value >= 0.;
}

inline bool density_modifier_option(const std::string &key)
{
    return key == "sparse_infill_density" || key == "voronoi_modifier_blend" ||
           key == "voronoi_wall_decay" || key == "voronoi_smoothing_sigma";
}

inline DynamicPrintConfig effective_density_modifier_config(const DynamicPrintConfig &config)
{
    if (!fused_density_modifier(config)) return config;
    DynamicPrintConfig effective;
    for (const auto &key : config.keys())
        if (density_modifier_option(key)) effective.set_key_value(key, config.option(key)->clone());
    return effective;
}

inline bool valid_density_modifier_blend(const DynamicPrintConfig &config)
{
    const auto *blend = config.option<ConfigOptionFloat>("voronoi_modifier_blend");
    return !blend || std::isfinite(blend->value);
}

} // namespace Slic3r::Voronoi
