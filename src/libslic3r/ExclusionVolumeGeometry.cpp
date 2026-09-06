#include "ExclusionVolumeGeometry.hpp"

#include "ClipperUtils.hpp"
#include "Print.hpp"

#include <algorithm>
#include <numeric>
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

std::vector<size_t> bed_exclusion_physical_extruders(
    const Print &print,
    const std::vector<unsigned int> &filament_ids,
    const size_t physical_extruder_count,
    const int layer_id)
{
    if (physical_extruder_count == 0)
        return {};

    std::vector<size_t> result;
    const auto grouping = print.get_layered_nozzle_group_result();
    const bool automatic_mapping_resolved =
        !is_auto_filament_map_mode(print.get_filament_map_mode()) || !print.is_BBL_printer() ||
        print.get_nozzle_group_result() != nullptr;
    bool unresolved = filament_ids.empty();

    for (const unsigned int filament_id : filament_ids) {
        if (grouping) {
            if (const auto nozzle = grouping->get_nozzle_for_filament(int(filament_id), layer_id);
                nozzle.has_value() && nozzle->extruder_id >= 0 &&
                size_t(nozzle->extruder_id) < physical_extruder_count) {
                result.emplace_back(size_t(nozzle->extruder_id));
                continue;
            }
        }

        const int resolved = bed_exclusion_extruder_for_filament(
            filament_id, print.get_filament_maps(), print.get_filament_map_mode(), print.is_BBL_printer(),
            automatic_mapping_resolved, physical_extruder_count);
        if (resolved >= 0)
            result.emplace_back(size_t(resolved));
        else
            unresolved = true;
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if (!unresolved && !result.empty())
        return result;

    result.resize(physical_extruder_count);
    std::iota(result.begin(), result.end(), size_t(0));
    return result;
}

ExPolygons active_bed_exclusion_footprints_for_object(
    const PrintObject &object,
    const std::vector<std::vector<BedExcludeRegion>> &regions_by_extruder,
    const std::vector<size_t> &physical_extruders,
    const double z_min,
    const double z_max,
    const coord_t clearance)
{
    ExPolygons result;
    for (const PrintInstance &instance : object.instances()) {
        const Point instance_shift = instance.shift_without_plate_offset();
        const Point shift(-instance_shift.x(), -instance_shift.y());
        expolygons_append(result, active_bed_exclusion_footprints(
            regions_by_extruder, physical_extruders, z_min, z_max, shift, clearance));
    }
    return result.empty() ? ExPolygons{} : union_ex(result);
}

} // namespace Slic3r
