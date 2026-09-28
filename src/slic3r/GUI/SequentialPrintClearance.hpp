#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r::GUI {

struct SequentialClearanceInstance
{
    double      instance_height;
    BoundingBox bounding_box;
    Polygon     hull_polygon;
};

// Keep model print order, as in Print::sequential_print_clearance_valid(). Sorting by
// X for overlapping Y ranges and by Y otherwise is not a strict weak ordering.
inline std::vector<std::pair<Polygon, float>> sequential_clearance_height_polygons(
    const std::vector<SequentialClearanceInstance>& instances, double printable_height, double height_to_lid, double height_to_rod)
{
    std::vector<std::pair<Polygon, float>> height_polygons;
    height_polygons.reserve(instances.size());
    for (size_t k = 0; k < instances.size(); ++k) {
        const auto& instance = instances[k];
        double height = k + 1 == instances.size() ? printable_height : height_to_lid;
        for (size_t i = k + 1; i < instances.size(); ++i) {
            const auto& next_bbox = instances[i].bounding_box;
            if (std::min(instance.bounding_box.max.y(), next_bbox.max.y()) >
                std::max(instance.bounding_box.min.y(), next_bbox.min.y())) {
                height = height_to_rod;
                break;
            }
        }
        if (height < instance.instance_height)
            height_polygons.emplace_back(instance.hull_polygon, float(height));
    }
    return height_polygons;
}

} // namespace Slic3r::GUI
