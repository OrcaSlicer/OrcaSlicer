#include "BeltFloorContext.hpp"

#include <cmath>
#include <limits>

namespace Slic3r {

bool BeltFloorContext::init(const SlicingParameters &sp, const PrintConfig &pcfg)
{
    m_active = false;
    m_shear_factor = sp.belt_floor_shear_factor;
    m_from_axis    = sp.belt_floor_from_axis;
    m_z_shift      = sp.belt_floor_z_shift;
    m_floor_offset = pcfg.belt_support_floor_offset.value;

    if (std::abs(m_shear_factor) < EPSILON)
        return false;

    m_active = true;
    return true;
}

bool BeltFloorContext::init_local(const SlicingParameters &sp, const PrintConfig &pcfg,
                                  double global_z_offset)
{
    if (!init(sp, pcfg))
        return false;
    // Local Z: subtract the global Z offset so polygon computation
    // works in the object's local coordinate space.
    m_z_shift -= global_z_offset;
    return true;
}

Polygons BeltFloorContext::surface_polygon(coordf_t print_z) const
{
    return half_plane(print_z, /*belt_surface=*/true);
}

Polygons BeltFloorContext::valid_region_polygon(coordf_t print_z) const
{
    return half_plane(print_z, /*belt_surface=*/false);
}

double BeltFloorContext::floor_print_z(const Point &pos_slicing) const
{
    if (!m_active)
        return -std::numeric_limits<double>::infinity();
    double pos = unscale<double>(m_from_axis == 0 ? pos_slicing.x() : pos_slicing.y());
    return m_shear_factor * pos + m_floor_offset + m_z_shift;
}

std::vector<Polygons> BeltFloorContext::compute_per_layer_floors(
    size_t num_layers,
    const std::function<double(size_t)> &layer_print_z) const
{
    std::vector<Polygons> result(num_layers);
    if (!m_active)
        return result;
    for (size_t i = 0; i < num_layers; ++i)
        result[i] = surface_polygon(layer_print_z(i));
    return result;
}

Polygons BeltFloorContext::half_plane(coordf_t print_z, bool belt_surface) const
{
    if (!m_active)
        return {};

    const double cutoff = (print_z - m_z_shift - m_floor_offset) / m_shear_factor;
    const coord_t cutoff_scaled = scale_(cutoff);
    const coord_t large_bound   = scale_(1e3);

    // The belt surface is on one side of the cutoff line; the valid region
    // is on the other side.  Which side depends on shear_factor sign.
    //
    // belt_surface=true  → the belt side (where support should NOT exist)
    // belt_surface=false → the valid side (where support IS allowed)
    //
    // For shear_factor > 0: belt surface is from_axis >= cutoff
    // For shear_factor < 0: belt surface is from_axis <= cutoff
    bool high_side = (m_shear_factor > 0) == belt_surface;

    Polygon poly;
    if (m_from_axis == 0) {
        if (high_side) {
            // X >= cutoff
            poly.points = {
                Point(cutoff_scaled, -large_bound),
                Point(large_bound,   -large_bound),
                Point(large_bound,    large_bound),
                Point(cutoff_scaled,  large_bound)
            };
        } else {
            // X < cutoff
            poly.points = {
                Point(-large_bound,  -large_bound),
                Point(cutoff_scaled, -large_bound),
                Point(cutoff_scaled,  large_bound),
                Point(-large_bound,   large_bound)
            };
        }
    } else {
        if (high_side) {
            // Y >= cutoff
            poly.points = {
                Point(-large_bound, cutoff_scaled),
                Point( large_bound, cutoff_scaled),
                Point( large_bound, large_bound),
                Point(-large_bound, large_bound)
            };
        } else {
            // Y < cutoff
            poly.points = {
                Point(-large_bound, -large_bound),
                Point( large_bound, -large_bound),
                Point( large_bound, cutoff_scaled),
                Point(-large_bound, cutoff_scaled)
            };
        }
    }
    return { poly };
}

} // namespace Slic3r
