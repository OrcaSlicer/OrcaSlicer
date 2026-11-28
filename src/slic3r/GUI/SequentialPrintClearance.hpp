#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Polygon.hpp"

#include <algorithm>
#include <cstddef>
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
    // Orca: the group the instance prints together with (group_objects_by_filament), size_t(-1) for none.
    size_t      group = size_t(-1);
};

inline bool same_sequential_clearance_group(const SequentialClearanceInstance& lhs, const SequentialClearanceInstance& rhs)
{
    return lhs.group != size_t(-1) && lhs.group == rhs.group;
}

// Orca: keep the instances of a group together, where the first of them is, as print validation orders them.
inline void gather_sequential_clearance_groups(std::vector<SequentialClearanceInstance>& instances)
{
    std::map<size_t, size_t> group_first;
    std::vector<std::pair<size_t, SequentialClearanceInstance>> ranked;
    ranked.reserve(instances.size());
    for (size_t k = 0; k < instances.size(); ++k) {
        const size_t group = instances[k].group;
        ranked.emplace_back(group == size_t(-1) ? k : group_first.try_emplace(group, k).first->second, std::move(instances[k]));
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (size_t k = 0; k < ranked.size(); ++k)
        instances[k] = std::move(ranked[k].second);
}

// Print validation may order the instances of an object differently from ModelObject::instances.
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
inline std::vector<std::pair<Polygon, float>> sequential_clearance_height_polygons(
    const std::vector<SequentialClearanceInstance>& instances, double printable_height, double height_to_lid, double height_to_rod)
{
    std::vector<std::pair<Polygon, float>> height_polygons;
    height_polygons.reserve(instances.size());
    for (size_t k = 0; k < instances.size(); ++k) {
        const auto& instance = instances[k];
        // Orca: the last group may reach the printable height, as the last instance does.
        double height = k + 1 == instances.size() || same_sequential_clearance_group(instance, instances.back()) ? printable_height : height_to_lid;
        for (size_t i = k + 1; i < instances.size(); ++i) {
            if (same_sequential_clearance_group(instance, instances[i]))
                continue; // printed together
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
