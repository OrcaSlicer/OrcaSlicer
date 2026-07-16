#pragma once

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {

bool bed_exclusion_z_ranges_overlap(double first_min, double first_max, double second_min, double second_max);

// Return the union of exclusion footprints intersecting the requested extrusion
// slab. Regions are already resolved into physical-nozzle/model coordinates by
// PrintConfig; callers only provide the physical nozzles that may emit the
// generated geometry and the translation into the caller's coordinate space.
ExPolygons active_bed_exclusion_footprints(
    const std::vector<BedExcludeRegion> &regions,
    double z_min,
    double z_max,
    const Point &translation = Point(0, 0),
    coord_t clearance = 0);

ExPolygons active_bed_exclusion_footprints(
    const std::vector<std::vector<BedExcludeRegion>> &regions_by_extruder,
    const std::vector<size_t> &physical_extruders,
    double z_min,
    double z_max,
    const Point &translation = Point(0, 0),
    coord_t clearance = 0);

} // namespace Slic3r
