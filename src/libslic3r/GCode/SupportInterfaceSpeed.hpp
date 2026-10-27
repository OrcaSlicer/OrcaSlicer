#pragma once

#include "../ClipperUtils.hpp"
#include "../Config.hpp"
#include "../ExPolygon.hpp"
#include "../Layer.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../Print.hpp"
#include "../libslic3r.h"
#include "ExtrusionProcessor.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace Slic3r {

// Project upper interfaces onto the first model surface above them. Account for
// the contact gap and one layer of rounding used by independently layered supports.
// Model/support material between the interface and the current layer occludes it.
inline ExPolygons support_interface_regions_below(const Layer &layer)
{
    const PrintObject &object = *layer.object();
    const double bottom_z = layer.bottom_z();
    ExPolygons regions;
    for (const SupportLayer *support : object.support_layers()) {
        if (support->print_z > bottom_z + EPSILON)
            break;
        // Support layers are generated independently from model layers. With
        // adaptive layer heights their Z rounding may therefore be larger than
        // the current model layer. Accept one layer of rounding from whichever
        // side is taller, otherwise upper support contacts may be missed.
        const double max_gap = object.config().support_top_z_distance.value +
                               std::max(layer.height, support->height);
        if (bottom_z - support->print_z > max_gap + EPSILON || support->top_interface_polygons.empty())
            continue;
        ExPolygons visible = union_ex(support->top_interface_polygons);
        for (const Layer *below : object.layers()) {
            if (below->print_z > bottom_z + EPSILON)
                break;
            if (below->print_z >= support->print_z - EPSILON)
                visible = diff_ex(visible, below->lslices);
        }
        for (const SupportLayer *above : object.support_layers()) {
            if (above->print_z > bottom_z + EPSILON)
                break;
            if (above->print_z > support->print_z + EPSILON) {
                visible = diff_ex(visible, above->support_islands);
                visible = diff_ex(visible, above->lslices);
                // Tree supports only populate lslices for brim/skirt layers.
                visible = diff_ex(visible, above->base_areas);
                visible = diff_ex(visible, above->top_interface_polygons);
            }
        }
        append(regions, std::move(visible));
    }
    return union_ex(regions);
}

struct SupportInterfaceLayerRegion
{
    ExPolygons regions;
    size_t     layer_offset { 0 };
};

inline std::vector<SupportInterfaceLayerRegion> support_interface_regions_for_slow_layers(const Layer &layer, size_t slow_layers)
{
    std::vector<SupportInterfaceLayerRegion> result;
    const auto &layers = layer.object()->layers();
    const auto current = std::find(layers.begin(), layers.end(), &layer);
    if (current == layers.end())
        return result;
    const size_t current_idx = size_t(std::distance(layers.begin(), current));
    for (size_t offset = 0; offset < slow_layers && offset <= current_idx; ++offset) {
        ExPolygons regions = support_interface_regions_below(*layers[current_idx - offset]);
        if (!regions.empty())
            result.push_back({std::move(regions), offset});
    }
    return result;
}

// ProcessedPoint::speed applies to the segment starting at that point. Clip each
// segment separately to preserve travel order, holes, Z and existing speed/fan data.
inline bool limit_speed_above_support(std::vector<ProcessedPoint> &points, const ExPolygons &regions,
                                     const ConfigOptionFloatOrPercent &limit, bool mark_for_fan = false,
                                     double blend_to_normal = 0.)
{
    if ((limit.value <= 0 && !mark_for_fan) || regions.empty() || points.size() < 2)
        return false;
    const Polygons polygons = to_polygons(regions);
    std::vector<ProcessedPoint> result;
    bool changed = false;
    for (size_t i = 1; i < points.size(); ++i) {
        const ProcessedPoint &a = points[i - 1];
        const ProcessedPoint &b = points[i];
        const Vec2d origin = a.p.to_point().cast<double>();
        const Vec2d delta = b.p.to_point().cast<double>() - origin;
        const double length_squared = delta.squaredNorm();
        const double first_layer_speed = limit.value > 0 ? std::min(double(a.speed), limit.get_abs_value(a.speed)) : a.speed;
        const double capped_speed = first_layer_speed + std::clamp(blend_to_normal, 0., 1.) * (double(a.speed) - first_layer_speed);
        if (length_squared == 0 || (capped_speed >= a.speed && !mark_for_fan)) {
            result.push_back(a);
            continue;
        }
        std::vector<std::pair<double, double>> intervals;
        std::vector<double> cuts{0., 1.};
        for (const Polyline &part : intersection_pl(Polyline(a.p.to_point(), b.p.to_point()), polygons)) {
            double start = std::clamp((part.first_point().cast<double>() - origin).dot(delta) / length_squared, 0., 1.);
            double end = std::clamp((part.last_point().cast<double>() - origin).dot(delta) / length_squared, 0., 1.);
            if (start > end)
                std::swap(start, end);
            if (end <= start)
                continue;
            intervals.emplace_back(start, end);
            cuts.push_back(start);
            cuts.push_back(end);
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        for (size_t j = 1; j < cuts.size(); ++j) {
            const double t = cuts[j - 1];
            const double mid = (t + cuts[j]) * 0.5;
            const bool above = std::any_of(intervals.begin(), intervals.end(), [mid](const auto &interval) {
                return mid >= interval.first && mid <= interval.second;
            });
            ProcessedPoint point = a;
            const Vec3d position = a.p.cast<double>() + t * (b.p.cast<double>() - a.p.cast<double>());
            point.p = Point3(position.x(), position.y(), position.z());
            point.overlap = float(a.overlap + t * (b.overlap - a.overlap));
            if (above) {
                point.speed = float(capped_speed);
                if (mark_for_fan && (!point.above_support_interface || blend_to_normal < point.above_support_blend_to_normal)) {
                    point.above_support_interface = true;
                    point.above_support_blend_to_normal = float(blend_to_normal);
                }
                changed = true;
            }
            result.push_back(point);
        }
    }
    if (changed) {
        result.push_back(points.back());
        points = std::move(result);
    }
    return changed;
}

} // namespace Slic3r
