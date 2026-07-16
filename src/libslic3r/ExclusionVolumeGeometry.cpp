#include "ExclusionVolumeGeometry.hpp"

#include "ClipperUtils.hpp"

#include <algorithm>
#include <utility>

namespace Slic3r {

namespace {

constexpr double Z_EPSILON_MM = 1e-6;

void append_active_footprints(
    Polygons &out,
    const std::vector<BedExcludeRegion> &regions,
    const double z_min,
    const double z_max,
    const Point &translation)
{
    for (const BedExcludeRegion &region : regions) {
        if (region.polygon.points.size() < 3 || region.z_max < region.z_min ||
            !bed_exclusion_z_ranges_overlap(z_min, z_max, region.z_min, region.z_max))
            continue;

        Polygon footprint = region.polygon;
        footprint.translate(translation);
        out.emplace_back(std::move(footprint));
    }
}

ExPolygons union_active_footprints(Polygons footprints, const coord_t clearance)
{
    if (footprints.empty())
        return {};

    ExPolygons result = union_ex(footprints);
    if (clearance > 0)
        result = offset_ex(result, float(clearance));
    return result;
}

} // namespace

bool bed_exclusion_z_ranges_overlap(
    double first_min,
    double first_max,
    double second_min,
    double second_max)
{
    if (first_min > first_max)
        std::swap(first_min, first_max);
    if (second_min > second_max)
        std::swap(second_min, second_max);
    return first_max >= second_min - Z_EPSILON_MM && first_min <= second_max + Z_EPSILON_MM;
}

ExPolygons active_bed_exclusion_footprints(
    const std::vector<BedExcludeRegion> &regions,
    const double z_min,
    const double z_max,
    const Point &translation,
    const coord_t clearance)
{
    Polygons footprints;
    footprints.reserve(regions.size());
    append_active_footprints(footprints, regions, z_min, z_max, translation);
    return union_active_footprints(std::move(footprints), clearance);
}

ExPolygons active_bed_exclusion_footprints(
    const std::vector<std::vector<BedExcludeRegion>> &regions_by_extruder,
    const std::vector<size_t> &physical_extruders,
    const double z_min,
    const double z_max,
    const Point &translation,
    const coord_t clearance)
{
    Polygons footprints;
    std::vector<bool> visited(regions_by_extruder.size(), false);
    for (const size_t extruder_id : physical_extruders) {
        if (extruder_id >= regions_by_extruder.size() || visited[extruder_id])
            continue;
        visited[extruder_id] = true;
        const std::vector<BedExcludeRegion> &regions = regions_by_extruder[extruder_id];
        footprints.reserve(footprints.size() + regions.size());
        append_active_footprints(footprints, regions, z_min, z_max, translation);
    }
    return union_active_footprints(std::move(footprints), clearance);
}

} // namespace Slic3r
