#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Polygon.hpp"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct SequentialClearanceInstance
{
    double      instance_height;
    BoundingBox bounding_box;
    Polygon     hull_polygon;
    ObjectID    instance_id;
};

// PrintApply may group rotated copies in a different order from ModelObject::instances.
// Reorder one object's instances only when validation has assigned all of them an order.
inline void sort_sequential_clearance_instances(
    std::vector<SequentialClearanceInstance>::iterator first, std::vector<SequentialClearanceInstance>::iterator last,
    const std::map<ObjectID, int>& print_order)
{
    if (!std::all_of(first, last, [&print_order](const SequentialClearanceInstance& instance) {
            const auto order = print_order.find(instance.instance_id);
            return order != print_order.end() && order->second > 0;
        }))
        return;

    std::stable_sort(first, last, [&print_order](const SequentialClearanceInstance& lhs, const SequentialClearanceInstance& rhs) {
        return print_order.at(lhs.instance_id) < print_order.at(rhs.instance_id);
    });
}

// Input follows object-list order with validated print order within each object.
// Sorting by X for overlapping Y ranges and by Y otherwise is not a strict weak ordering.
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
