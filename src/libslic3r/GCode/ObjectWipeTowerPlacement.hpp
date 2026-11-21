#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace Slic3r {

class PrintConfig;

struct ObjectWipeTowerPlacement
{
    Vec2f   position;  // Plate-local translation, as used by WipeTowerIntegration.
    Polygon footprint; // Includes brim, in model/plate coordinates.
    double  height = 0.;
};

// All polygons and shifts use scaled model/plate coordinates; tower height and
// compacted rise are in mm. Vector order is the sequential print order.
struct ObjectWipeTowerPlacementInstance
{
    Polygon footprint;
    Polygon model_hull; // Rotated, but not yet translated or offset.
    Point   shift;
    double  top = 0.;   // Scaled maximum object Z.
    Polygon tower_hull; // Empty for instances that do not need a tower.
    double  tower_height   = 0.;
    bool    compacted      = false;
    double  compacted_rise = 0.;
};

struct SequentialClearanceGeometry
{
    Polygon     hull;
    Polygon     expanded;
    BoundingBox rod_box;
    double      top;
};

// Offset the local hull before translation (including the existing empty-offset
// fallback), then optionally widen the unit by its tower. The hull is built the way
// Print::sequential_print_clearance_valid() builds it.
SequentialClearanceGeometry sequential_clearance_geometry(
    const Polygon &local_hull, const Point &shift, float object_distance, double rod_adjustment, double top, const Polygon *tower = nullptr);
SequentialClearanceGeometry sequential_clearance_with_tower(const SequentialClearanceGeometry &base, const Polygon &tower, float object_distance, double rod_adjustment);
bool                        sequential_clearance_overlaps_in_y(const BoundingBox &a, const BoundingBox &b);

struct ObjectWipeTowerPlacementParameters
{
    Polygons                 bed; // Shared printable area, before subtracting excluded areas.
    Polygons                 excluded;
    Point                    origin;
    double                   clearance;
    float                    object_distance;
    double                   rod_adjustment;
    double                   lid_height; // Scaled Z limits.
    double                   rod_height;
    double                   printable_height;
    const PrintConfig       *compacted_config = nullptr; // Borrowed for the duration of the search.
};

struct ObjectWipeTowerPlacementResult
{
    std::vector<std::pair<size_t, ObjectWipeTowerPlacement>> placed;
    std::vector<size_t>                                      unplaced; // Only instances that need a tower, in print order.
};

// Deterministic partial placement: maximum count, then earlier instances, then
// candidate rank, within the fixed search budget. Cancellation may throw.
ObjectWipeTowerPlacementResult place_object_wipe_towers(const std::vector<ObjectWipeTowerPlacementInstance> &instances,
                                                        const ObjectWipeTowerPlacementParameters            &parameters,
                                                        const std::function<void()>                         &throw_if_canceled);

} // namespace Slic3r
